#include <algorithm>
#include <cmath>
#include <string_view>

#include "sky_whale_graphics.hpp"
#include "body_parts.hpp"
#include "creature.hpp"
#include "rooms.hpp"
#include "sky_whale.hpp"

namespace sky_whale_graphics {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using world::room_view;
    using sprites::sprite;
    using namespace sky_whale;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        // SkyWhale's chunk radii (times CreatureParams.sizeMultiplier) and
        // its BodyChunkConnections' 15 (the normal ones)
        constexpr float chunk_rads[chunk_count] = {8.f, 10.f, 10.5f, 10.f, 9.f, 8.f};
        constexpr float connection = 15.f;
        constexpr uint32_t last = smooth_count - 1;
        // TailSegment(rad, tailSegSeparation, prev, 0.85, 0.95, 0.4, false)
        constexpr float tail_surface_friction = 0.85f, tail_air_friction = 0.95f, tail_affect_previous = 0.4f;
        // the CreatureLooker's framesBetweenFocusChange and
        // chanceToLookAtNothing; the template's visualRadius
        constexpr float frames_between_focus_change = 70.f;
        constexpr float chance_to_look_at_nothing = 0.5f;
        constexpr float visual_radius = 1100.f;

        // the markings' elements and their sourceRect sizes (Marking's
        // width and height come from the atlas element)
        struct element {
            std::string_view name;
            float w, h;
        };
        constexpr element marking_elements[] = {
            {"tinyStar", 3.f, 3.f},
            {"scarA0", 20.f, 5.f}, {"scarA1", 17.f, 2.f}, {"scarA2", 17.f, 4.f}, {"scarA3", 22.f, 4.f}, {"scarA4", 23.f, 5.f},
            {"scarB0", 3.f, 3.f}, {"scarB1", 5.f, 4.f}, {"scarB2", 5.f, 5.f}, {"scarB3", 5.f, 5.f}, {"scarB4", 4.f, 4.f},
            {"Pebble5", 10.f, 11.f}, {"Pebble6", 9.f, 10.f}, {"Pebble7", 12.f, 10.f}, {"Pebble8", 10.f, 10.f},
            {"Circle4", 4.f, 4.f},
        };

        graphics_state& gs(sys::game_state& state, dcon::creature_id c) {
            return state.world.sky_whale_data_get_graphics(creature::sky_whale_data(state.world, c));
        }
        graphics_state const& gs(sys::game_state const& state, dcon::creature_id c) {
            return state.world.sky_whale_data_get_graphics(creature::sky_whale_data(state.world, c));
        }
        graphics_layout const& layout(sys::game_state const& state, dcon::creature_id c) {
            return state.world.sky_whale_data_get_layout(creature::sky_whale_data(state.world, c));
        }
        vec2 cpos(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_pos(sky_whale::chunk(state, c, i));
        }
        float crad(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_rad(sky_whale::chunk(state, c, i));
        }
        vec3 lerp3(vec3 a, vec3 b, float t) {
            t = mathf::clamp01(t);
            return a + (b - a) * t;
        }
        vec4 v4(vec3 c, float a = 1.f) { return vec4{c.x, c.y, c.z, a}; }

        // SegmentFromDist (capped at the last segment: vanilla would index
        // past it)
        float segment_from_dist(graphics_layout const& L, float dist) {
            for (uint32_t i = 1; i < smooth_count; ++i)
                if (L.seg_dist[i] > dist) {
                    float const num = (dist - L.seg_dist[i - 1]) / (L.seg_dist[i] - L.seg_dist[i - 1]);
                    return float(i - 1) + num;
                }
            return float(last);
        }
        // GetUndersideAngle
        float underside_angle(bool alt, float seg) {
            float const num = seg / float(smooth_count);
            float num2 = 0.7853982f * std::sqrt(mathf::min(num * num * 10.f, 1.f));
            if (alt)
                num2 *= 1.5f * std::sqrt(mathf::inverse_lerp(0.85f, 0.65f, num));
            return num2;
        }

        // the unsmoothed segments: the chunks, then the tail
        vec2 unsmoothed_pos(sys::game_state const& state, dcon::creature_id c, graphics_state const& g, uint32_t i) {
            return i < chunk_count ? cpos(state, c, i) : g.tail[i - chunk_count].pos;
        }

        // the first half of Update: the smoothed body from the chunks and
        // the tail
        void smooth_body(sys::game_state const& state, dcon::creature_id c, graphics_state& g, graphics_layout const& L) {
            auto const& x = state.world.creature_get_payload(c).sky_whale;
            for (uint32_t j = snout_segments; j < smooth_count; j += 2) {
                uint32_t const num = (j - snout_segments) / 2;
                vec2 const u = unsmoothed_pos(state, c, g, num);
                g.smooth[j].pos = u;
                if (j + 1 < smooth_count)
                    g.smooth[j + 1].pos = (u + unsmoothed_pos(state, c, g, num + 1)) / 2.f;
            }
            vec2 const c0 = cpos(state, c, 0);
            vec2 const n = rw::normalized(c0 - cpos(state, c, 1));
            float const r0 = crad(state, c, 0);
            for (uint32_t k = 0; k < snout_segments; ++k) {
                float const num2 = 1.f - float(k) / float(snout_segments);
                g.smooth[k].pos = c0 + n * r0 * std::sin(num2 * mathf::pi / 2.f);
            }
            vec2 a = g.smooth[0].pos;
            for (uint32_t l = 1; l < last; ++l) {
                vec2 const p = g.smooth[l].pos;
                vec2 const b = (a + g.smooth[l + 1].pos) / 2.f;
                g.smooth[l].pos = (p + b) / 2.f;
                a = p;
            }
            if (x.alt) {
                float const body_length = L.seg_dist[last];
                vec2 a2 = g.smooth[0].pos;
                for (uint32_t m = 0; m < last; ++m) {
                    vec2 const p2 = g.smooth[m].pos;
                    vec2 const a3 = custom::perpendicular_vector(rw::normalized(a2 - g.smooth[m + 1].pos));
                    float const num3 = mathf::min(L.seg_dist[m] / body_length * 6.f, 1.f);
                    float const d = mathf::pow(1.f - num3, 1.5f);
                    g.smooth[m].pos -= a3 * d * std::sin(x.roll * mathf::pi / 2.f) * x.size * 3.f;
                    a2 = p2;
                }
            }
        }

        // the body at the tick's positions (timeStacker 1) for the fins
        vec2 body_dir_now(graphics_state const& g, uint32_t s) {
            return rw::normalized(g.smooth[s > 0 ? s - 1 : 0].pos - g.smooth[std::min(s + 1, last)].pos);
        }
        vec2 point_on_body_now(graphics_state const& g, graphics_layout const& L, uint32_t s, float rfu, float roll, float offset) {
            vec2 const a = custom::perpendicular_vector(body_dir_now(g, s));
            return g.smooth[s].pos + a * (L.seg_rad[s] + offset) * std::sin(roll * mathf::pi / 2.f + rfu);
        }

        // the fins' Update and their placement
        void place_fins(sys::game_state const& state, dcon::creature_id c, graphics_state& g, graphics_layout const& L, bool reset) {
            auto const& x = state.world.creature_get_payload(c).sky_whale;
            for (uint32_t n = 0; n < fin_rows; ++n)
                for (uint32_t k = 0; k < 2; ++k) {
                    auto& f = g.fins[n][k];
                    uint32_t const s = L.fin_segment[n][k];
                    float const att = L.fin_attachment_angle[n][k];
                    float const len = L.fin_length[n][k];
                    // GetFinAttachmentPos: 10 into the body
                    vec2 const at = point_on_body_now(g, L, s, att, x.roll, -10.f);
                    vec2 const n2 = rw::normalized(g.smooth[s - 1].pos - g.smooth[s + 1].pos);
                    vec2 const a4 = custom::perpendicular_vector(n2);
                    // Fin.Update (its velocity's ConnectToPoint is overridden by
                    // the placement)
                    f.last_angle = f.angle;
                    f.last_pos = f.pos;
                    f.angle = att + std::sin(float(g.swim_timer) / 30.f * mathf::pi - float(n) * mathf::pi / 4.f) * mathf::sign(att) * 0.5f;
                    f.pos = at - n2 * len * 0.7f + a4 * std::sin(x.roll * mathf::pi / 2.f + f.angle) * len * 0.7f;
                    if (reset) {
                        f.last_pos = f.pos;
                        f.last_angle = f.angle;
                    }
                }
        }

        // TailSegment.Update
        void tail_update(player::body_part& bp, float connection_rad, player::body_part* prev, vec2 point, room_view const& room) {
            bp.last_pos = bp.pos;
            bp.pos += bp.vel;
            bp.vel *= tail_air_friction;
            bp.stretched = 1.f;
            if (prev) {
                if (!custom::dist_less(bp.pos, prev->pos, connection_rad)) {
                    float const num = rw::distance(bp.pos, prev->pos);
                    vec2 const a = custom::dir_vec(bp.pos, prev->pos);
                    bp.pos -= a * (connection_rad - num) * (1.f - tail_affect_previous);
                    bp.vel -= a * (connection_rad - num) * (1.f - tail_affect_previous);
                    prev->vel += a * (connection_rad - num) * tail_affect_previous;
                    bp.stretched = mathf::clamp((connection_rad / (num * 0.5f) + 2.f) / 3.f, 0.2f, 1.f);
                }
            } else if (!custom::dist_less(bp.pos, point, connection_rad)) {
                float const num = rw::distance(bp.pos, point);
                vec2 const a = custom::dir_vec(bp.pos, point);
                bp.pos -= a * (connection_rad - num);
                bp.vel -= a * (connection_rad - num);
                bp.stretched = mathf::clamp((connection_rad / (num * 0.5f) + 2.f) / 3.f, 0.2f, 1.f);
            }
            body_parts::push_out_of_terrain(bp, room, tail_surface_friction);
        }

        // CreatureLooker (simplified): now and then it looks at the closest
        // creature it can see, or at nothing
        void look_update(sys::game_state& state, dcon::creature_id c, graphics_state& g, room_view const& room) {
            auto& rng = state.world.creature_get_graphics_random(c);
            vec2 const eye = cpos(state, c, 0);
            if (g.look && (!creature::here(state, g.look, room.id) || !custom::dist_less(eye, creature::main_pos(state, g.look), visual_radius)))
                g.look = dcon::creature_id{};
            if (g.look_counter > 0)
                --g.look_counter;
            if (g.look_counter < 1 && rng.value() < 1.f / frames_between_focus_change) {
                g.look_counter = 20;
                g.look = dcon::creature_id{};
                if (rng.value() > chance_to_look_at_nothing) {
                    float best = visual_radius;
                    rooms::for_each_creature(state, room.id, [&](dcon::creature_id o) {
                        if (o == c || !creature::here(state, o, room.id) || creature::dead(state, o))
                            return;
                        if (creature::is(state.world, o, creature::type::sky_whale))
                            return;
                        vec2 const p = creature::main_pos(state, o);
                        float const d = rw::distance(eye, p);
                        if (d < best && room.visual_contact(eye, p)) {
                            best = d;
                            g.look = o;
                        }
                    });
                }
            }
            g.has_look = 0;
            if (g.look) {
                g.look_pos = creature::main_pos(state, g.look);
                g.has_look = 1;
            }
        }
    }

    graphics_layout make_layout(creature_params const& p, bool alt, int32_t seed) {
        graphics_layout L;
        float const size = p.size_multiplier;
        float cr[chunk_count];
        for (uint32_t i = 0; i < chunk_count; ++i)
            cr[i] = chunk_rads[i] * size;
        float const conn = connection * size;
        L.tail_seg_separation = conn * 0.7f;
        for (uint32_t i = 0; i < tail_count; ++i) {
            float const num = float(i + 1) / float(tail_count);
            L.tail_rad[i] = (1.f - num) * cr[chunk_count - 1] + mathf::pow(num, 3.f) * 7.f;
        }
        auto const ur = [&](uint32_t i) { return i < chunk_count ? cr[i] : L.tail_rad[i - chunk_count]; };
        // the snout's quarter circle
        for (uint32_t l = 0; l < snout_segments; ++l) {
            float const num4 = (cr[0] - cr[1]) / conn;
            float const f = (1.f - float(l) / float(snout_segments)) * mathf::pi / 2.f;
            float const rad = cr[0];
            float const num5 = std::sin(f) * rad;
            L.seg_rad[l] = (rad + num4 * num5) * std::cos(f);
            L.seg_dist[l] = rad - num5;
        }
        float num6 = cr[0];
        for (uint32_t m = snout_segments; m < smooth_count; m += 2) {
            uint32_t const num7 = (m - snout_segments) / 2;
            float const num8 = num7 < chunk_count - 1 ? conn : L.tail_seg_separation;
            L.seg_rad[m] = ur(num7);
            L.seg_dist[m] = num6;
            if (m + 1 < smooth_count) {
                L.seg_rad[m + 1] = (ur(num7) + ur(num7 + 1)) / 2.f;
                L.seg_dist[m + 1] = num6 + num8 / 2.f;
            }
            num6 += num8;
        }
        float const body_length = L.seg_dist[last];
        if (alt)
            for (uint32_t n = 0; n < smooth_count; ++n) {
                float const num9 = mathf::min(L.seg_dist[n] / body_length * 6.f, 1.f);
                L.seg_rad[n] *= 1.f - 0.5f * mathf::pow(1.f - num9, 2.f);
            }
        // the fins
        for (uint32_t j = 0; j < fin_rows; ++j)
            for (uint32_t k = 0; k < 2; ++k) {
                float const num2 = float(j) / float(fin_rows - 1);
                float num3 = 150.f * mathf::lerp(0.5f, 1.f, 4.f * num2 * (1.f - num2));
                if (alt)
                    num3 *= 0.4f;
                L.fin_segment[j][k] = uint8_t(j * 2 + 2 + snout_segments);
                L.fin_length[j][k] = num3;
                float const side = float(k) * 2.f - 1.f;
                L.fin_attachment_angle[j][k] = mathf::pi * (alt ? 0.87f : 0.6f) * -side;
            }
        // the constructor's rolls, in its Random state (the ID's seed)
        rw::unity_random r{seed};
        L.swim_timer_start = r.range(0, 1000);
        auto const add = [&](float segment, float angle, float rotation, marking_sprite s, marking_color col) -> marking* {
            if (L.marking_count >= max_markings)
                return nullptr;
            auto& m = L.markings[L.marking_count++];
            m.segment = mathf::clamp(segment, 0.f, float(last));
            m.angle = angle;
            m.sprite_rotation = rotation;
            m.sprite = s;
            m.color = col;
            m.width = marking_elements[uint32_t(s)].w;
            m.height = marking_elements[uint32_t(s)].h;
            return &m;
        };
        if (alt) {
            int32_t const num11 = r.range(80, 120);
            for (int32_t i = 0; i < num11; ++i) {
                float const num13 = segment_from_dist(L, r.range(40.f, body_length));
                float num14 = mathf::pi - underside_angle(alt, num13);
                float const num15 = mathf::pow(r.value(), 2.f);
                num14 -= num15 * 1.5f;
                if (r.value() < 0.5f)
                    num14 = -num14;
                marking_sprite s = marking_sprite::circle4;
                if (r.value() < 1.f - num15 * 1.5f)
                    s = marking_sprite(uint32_t(marking_sprite::pebble5) + uint32_t(r.range(5, 9) - 5));
                add(num13, num14, r.range(-180.f, 180.f), s, marking_color::scar);
            }
        } else {
            if (r.value() < mathf::pow(1.f - p.ruggedness, 2.f)) {
                // AddFreckles
                float const t = r.value() < 0.5f ? 0.f : r.value();
                int32_t const num = r.range(20, 60);
                for (int32_t i = 0; i < num; ++i) {
                    float num2 = segment_from_dist(L, r.value() * body_length);
                    num2 = mathf::lerp(num2, std::round(num2), t);
                    float const rfu = r.range(-120.f, 120.f) * mathf::deg2rad;
                    add(num2, rfu, r.range(-180.f, 180.f), marking_sprite::tiny_star, marking_color::light_skin);
                }
            }
            if (r.value() < p.ruggedness * 3.f) {
                // AddScars
                int32_t const num = int32_t(r.range(30.f, 80.f) * p.ruggedness);
                for (int32_t i = 0; i < num; ++i) {
                    float const num2 = mathf::pow(r.value(), 1.7f) * body_length * 0.6f;
                    float const seg = segment_from_dist(L, num2);
                    float const rfu = r.range(-150.f, 150.f) * mathf::deg2rad;
                    bool const flag = r.value() < mathf::inverse_lerp(15.f, 120.f, num2) * 0.8f;
                    int32_t const num3 = r.range(0, 5);
                    auto const s = marking_sprite(uint32_t(flag ? marking_sprite::scar_a0 : marking_sprite::scar_b0) + uint32_t(num3));
                    auto* m = add(seg, rfu, r.range(-180.f, 180.f), s, marking_color::scar);
                    float const wf = r.range(0.6f, 1.f);
                    float const hf = r.range(0.6f, 1.f);
                    if (m) {
                        m->width *= wf;
                        m->height *= hf;
                    }
                }
            }
        }
        // GenerateArmor (reversed)
        if (!alt && r.value() < 0.4f * p.ruggedness + 0.2f) {
            float const num = r.range(50.f, 100.f);
            int32_t const n = r.range(4, 9);
            L.armor_count = uint8_t(std::min(n, int32_t(max_armor)));
            for (int32_t i = 0; i < n && i < int32_t(max_armor); ++i) {
                float const num2 = float(i) / float(n - 1);
                float const num3 = mathf::lerp(35.f, 25.f, mathf::pow(std::fabs(1.f - 2.f * num2), 3.f));
                float const seg = mathf::clamp(segment_from_dist(L, num + 32.f * float(i) - num3 / 2.f), 0.f, float(last));
                float const angle = (40.f + 202.5f * num2 * mathf::pow(1.f - num2, 2.f)) * mathf::deg2rad;
                L.armor[uint32_t(n - 1 - i)] = armor_plate{seg, angle, num3};
            }
        }
        L.eye_on_body_angle = alt ? 2.042035f : (110.f + r.range(-15.f, 15.f)) * mathf::deg2rad;
        L.eye_on_body_segment = float(snout_segments) + 1.2f + r.range(-0.5f, 0.5f);
        return L;
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        auto& g = gs(state, c);
        auto const& L = layout(state, c);
        int32_t const swim = g.swim_timer != 0 ? g.swim_timer : L.swim_timer_start;
        g = graphics_state{};
        g.swim_timer = swim;
        // SkyWhaleGraphics.Reset: the tail out behind the last chunk
        vec2 const p = cpos(state, c, chunk_count - 1);
        vec2 const n = rw::normalized(p - cpos(state, c, chunk_count - 2));
        for (uint32_t i = 0; i < tail_count; ++i) {
            auto& t = g.tail[i];
            t.pos = t.last_pos = p + float(i + 1) * L.tail_seg_separation * n;
            t.vel = vec2{0.f, 0.f};
            t.rad = L.tail_rad[i];
        }
        smooth_body(state, c, g, L);
        for (auto& s : g.smooth)
            s.last_pos = s.pos;
        place_fins(state, c, g, L, true);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        if (sky_whale::hidden(state, c))
            return;
        auto& g = gs(state, c);
        auto const& L = layout(state, c);
        auto& rng = w.creature_get_graphics_random(c);
        bool const awake = sky_whale::consious(state, c);
        look_update(state, c, g, room);
        if (g.blink > 0)
            --g.blink;
        g.blink = std::max(g.blink, w.creature_get_stun(c));
        if (!awake)
            g.blink = std::max(g.blink, 1);
        // (Sky_Whale_Fin_Flap every 60 ticks has no sound id)
        if (awake)
            ++g.swim_timer;
        if (g.look != g.last_look) {
            g.last_look = g.look;
            g.blink = std::max(g.blink, rng.range(5, 20));
        }
        for (auto& s : g.smooth)
            s.last_pos = s.pos;
        smooth_body(state, c, g, L);
        place_fins(state, c, g, L, false);
        // the tail after the last chunk
        vec2 const end = cpos(state, c, chunk_count - 1);
        for (uint32_t i = 0; i < tail_count; ++i)
            tail_update(g.tail[i], L.tail_seg_separation, i > 0 ? &g.tail[i - 1] : nullptr, end, room);
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip) {
        auto const& w = state.world;
        if (sky_whale::hidden(state, c))
            return;
        auto const& g = gs(state, c);
        auto const& L = layout(state, c);
        auto const& x = w.creature_get_payload(c).sky_whale;
        auto const& ws = w.sky_whale_data_get_whiskers(creature::sky_whale_data(w, c));
        auto const& par = sky_whale::params(state, c);
        bool const alt = x.alt != 0;
        float const t = alpha;

        // ApplyPalette
        vec3 const black = pal.black();
        float const fog_amount = 1.f - pal.texel(9, 7).x;
        vec3 const bg = lerp3(pal.sky(), pal.fog(), fog_amount);
        vec3 skin, underside;
        if (alt) {
            skin = vec3{0.17f, 0.17f, 0.17f};
            underside = vec3{0.46f, 0.4f, 0.29f};
        } else {
            skin = lerp3(black, bg, 0.1f);
            underside = lerp3(lerp3(black, vec3{1.f, 1.f, 1.f}, 0.5f), bg, 0.5f);
        }

        // the smoothed body between ticks
        vec2 dp[smooth_count];
        for (uint32_t i = 0; i < smooth_count; ++i)
            dp[i] = rw::lerp(g.smooth[i].last_pos, g.smooth[i].pos, t);
        float const num = mathf::lerp(x.last_roll, x.roll, t);
        float const num2 = std::sin(num * mathf::pi / 2.f);
        auto const body_dir = [&](uint32_t s) { return rw::normalized(dp[s > 0 ? s - 1 : 0] - dp[std::min(s + 1, last)]); };
        auto const point_on = [&](uint32_t s, float rfu, float offset, bool absolute) {
            vec2 const a = custom::perpendicular_vector(body_dir(s));
            float const roll = absolute ? 0.f : num * mathf::pi / 2.f;
            return dp[s] + a * (L.seg_rad[s] + offset) * std::sin(roll + rfu);
        };
        auto const point_on_f = [&](float s, float rfu, float offset, bool absolute) {
            s = mathf::clamp(s, 0.f, float(last));
            uint32_t const i = uint32_t(s);
            return rw::lerp(point_on(i, rfu, offset, absolute), point_on(std::min(i + 1, last), rfu, offset, absolute), s - float(i));
        };
        // (GetBodyDir(float)'s Slerp is between the same segment twice)
        auto const body_dir_f = [&](float s) { return body_dir(uint32_t(mathf::clamp(s, 0.f, float(last)))); };

        // the fins and whiskers, each in the container of its side
        struct piece {
            float z;
            bool fin;
            uint32_t a, b;
        };
        piece back[fin_rows * 2 + whisker_count];
        piece front[fin_rows * 2 + whisker_count];
        uint32_t nb = 0, nf = 0;
        for (uint32_t n = 0; n < fin_rows; ++n)
            for (uint32_t k = 0; k < 2; ++k) {
                // Fin.InBackground; sortZ = side * 5 - row
                bool const in_back = std::cos(x.roll * mathf::pi / 2.f + L.fin_attachment_angle[n][k]) < 0.f;
                piece const p{float(k * fin_rows) - float(n), true, n, k};
                (in_back ? back[nb++] : front[nf++]) = p;
            }
        for (uint32_t i = 0; i < ws.count; ++i) {
            // SkyWhaleWhisker.BackgroundFactor > 0
            float const bf = mathf::inverse_lerp(-0.1f, -0.4f, std::cos(x.roll * mathf::pi / 2.f + ws.w[i].side * mathf::pi / 2.f));
            piece const p{0.f, false, i, 0};
            (bf > 0.f ? back[nb++] : front[nf++]) = p;
        }
        auto const by_z = [](piece const& a, piece const& b) { return a.z < b.z; };
        std::stable_sort(back, back + nb, by_z);
        std::stable_sort(front, front + nf, by_z);

        auto const emit_fin = [&](uint32_t n, uint32_t k) {
            uint32_t const s = L.fin_segment[n][k];
            float const att = L.fin_attachment_angle[n][k];
            float const len = L.fin_length[n][k];
            auto const& f = g.fins[n][k];
            vec2 const at = point_on(s, att, -10.f, false);
            vec2 const v4p = rw::lerp(f.last_pos, f.pos, t);
            vec2 v5 = custom::perpendicular_vector(rw::normalized(v4p - at));
            vec2 const nrm = rw::normalized(dp[s - 1] - dp[s + 1]);
            vec2 const v6 = custom::perpendicular_vector(nrm) * std::sin(num * mathf::pi / 2.f + att);
            if (rw::dot(v5, v6) > 0.f)
                v5 = -v5;
            constexpr uint32_t nv = 24;
            vec2 vs[nv];
            vec4 cols[nv];
            vec3 const ca = alt ? underside : skin;
            vec3 const cb = alt ? skin : underside;
            for (uint32_t m = 0; m < nv; m += 2) {
                float const f14 = float(m) / float(nv - 2);
                vec2 const a8 = custom::bezier(at, at + v6 * len * 0.7f - nrm * len * 0.2f * (alt ? 0.5f : 1.f), v4p,
                    v4p + v6 * len * 0.2f * (1.f - par.fin_sharpness), f14);
                float const d2 = (1.f - f14) * 4.f;
                float num15 = len * mathf::lerp(2.f * f14 * (1.f - f14), 4.f * f14 * mathf::pow(1.f - f14, 2.f), par.fin_sharpness * 0.8f);
                if (alt)
                    num15 *= 0.1f + mathf::pow(f14, 0.7f) * 0.9f;
                vs[m] = a8 - v5 * d2;
                vs[m + 1] = a8 + v5 * d2 - v6 * num15;
                cols[m] = cols[m + 1] = v4(lerp3(ca, cb, mathf::pow(f14, 5.f) * 0.5f));
            }
            sprites::long_mesh(out, strip, vs, cols, nv);
        };
        auto const emit_whisker = [&](uint32_t i) {
            auto const& wh = ws.w[i];
            uint32_t const n = wh.segments;
            if (n < 2)
                return;
            vec2 vs[(max_whisker_segments - 1) * 4 + 3];
            uint32_t const nv = (n - 1) * 4 + 3;
            auto const seg_pos = [&](uint32_t k) { return rw::lerp(wh.last_pos[k], wh.pos[k], t); };
            vec2 v9 = seg_pos(0);
            v9 += rw::normalized(v9 - seg_pos(1));
            float num23 = 1.5f;
            for (uint32_t k = 0; k < n; ++k) {
                float const tt = float(k) / float(n - 1);
                vec2 const v10 = seg_pos(k);
                vec2 const a14 = custom::perpendicular_vector(rw::normalized(v10 - v9));
                float const num25 = mathf::lerp(wh.start_radius, wh.end_radius, tt);
                vs[k * 4] = v10 - a14 * (num23 + num25) * 0.5f;
                vs[k * 4 + 1] = v10 + a14 * (num23 + num25) * 0.5f;
                if (k < n - 1) {
                    vs[k * 4 + 2] = v10 - a14 * num25;
                    vs[k * 4 + 3] = v10 + a14 * num25;
                } else {
                    vs[k * 4 + 2] = v10;
                }
                num23 = num25;
                v9 = v10;
            }
            float const bf = mathf::inverse_lerp(-0.1f, -0.4f, std::cos(x.roll * mathf::pi / 2.f + wh.side * mathf::pi / 2.f));
            sprites::long_mesh(out, strip, vs, nv, lerp3(skin, bg, bf * 0.5f));
        };
        auto const emit = [&](piece const& p) {
            if (p.fin)
                emit_fin(p.a, p.b);
            else
                emit_whisker(p.a);
        };

        // containers[0]: what is behind the body
        for (uint32_t i = 0; i < nb; ++i)
            emit(back[i]);

        // the body (SkyWhaleBody shader: flat here) and its underside
        {
            vec2 vs[smooth_count * 2];
            for (uint32_t j = 0; j < smooth_count; ++j) {
                vec2 const a4 = custom::perpendicular_vector(dp[j == 0 ? 0 : j - 1] - dp[j == last ? j : j + 1]);
                vs[j * 2] = dp[j] + a4 * L.seg_rad[j];
                vs[j * 2 + 1] = dp[j] - a4 * L.seg_rad[j];
            }
            sprites::long_mesh(out, strip, vs, smooth_count * 2, skin);
            for (uint32_t i = 0; i < smooth_count; ++i) {
                vec2 const a2 = custom::perpendicular_vector(dp[i == 0 ? 0 : i - 1] - dp[i == last ? i : i + 1]);
                float const rad = L.seg_rad[i];
                float num4 = underside_angle(alt, float(i));
                if (num2 < 0.f)
                    num4 = -num4;
                float d;
                if (std::cos(num * mathf::pi / 2.f + num4) > 0.f)
                    d = mathf::sign(num4);
                else
                    d = std::sin(num * mathf::pi / 2.f + num4);
                vs[i * 2] = dp[i] - mathf::sign(num4) * a2 * rad;
                vs[i * 2 + 1] = dp[i] - d * a2 * rad;
            }
            sprites::long_mesh(out, strip, vs, smooth_count * 2, underside);
        }

        // the tail flukes: MakeLongMesh(5)
        {
            float const d3 = x.size * 20.f;
            float const num16 = x.size * 7.f;
            float const num18 = num * 0.9f;
            vec2 const a9 = dp[last];
            vec2 const v7 = -body_dir(last);
            vec2 const a10 = custom::perpendicular_vector(v7) * std::cos(num18 * mathf::pi / 2.f);
            constexpr uint32_t nv = 20;
            for (uint32_t k = 0; k < 2; ++k) {
                vec2 const v8 = (v7 * std::sin(0.6981317f) + a10 * std::sin(0.6981317f) * (float(k) * 2.f - 1.f)) * d3;
                vec2 const a11 = custom::perpendicular_vector(v8);
                vec2 vs[nv];
                vec4 cols[nv];
                for (uint32_t m = 0; m < nv; m += 2) {
                    float const f = float(m) / float(nv - 2);
                    vec2 const a12 = a9 + v8 * f;
                    float const a13 = mathf::lerp(L.seg_rad[last], std::cos(f * mathf::pi / 2.f) * num16, f);
                    float const b = mathf::lerp(L.seg_rad[last], std::cos(f * mathf::pi / 2.f) * num16 * 0.5f, f);
                    float const d4 = mathf::lerp(a13, b, std::fabs(std::sin(num18 * mathf::pi / 2.f)));
                    vs[m] = a12 - a11 * d4;
                    vs[m + 1] = a12 + a11 * d4;
                    cols[m] = cols[m + 1] = v4(lerp3(skin, underside, mathf::pow(f, 5.f) * 0.5f));
                }
                sprites::long_mesh(out, strip, vs, cols, nv);
            }
        }

        // the markings (CustomFSprites, on the side facing the camera)
        for (uint32_t k = 0; k < L.marking_count; ++k) {
            auto const& m = L.markings[k];
            float const facing = std::cos(num * mathf::pi / 2.f + m.angle);
            if (facing < 0.f)
                continue;
            vec2 const a5 = point_on_f(m.segment, m.angle, 0.f, false);
            vec2 const bd = body_dir_f(m.segment);
            vec2 const a6 = custom::perpendicular_vector(bd) * facing;
            float const n5 = std::cos(m.sprite_rotation * mathf::deg2rad);
            float const n6 = std::sin(m.sprite_rotation * mathf::deg2rad);
            vec2 const vv = m.width / 2.f * vec2{n5, -n6};
            vec2 const v2 = m.height / 2.f * vec2{n6, n5};
            vec2 const q[4] = {
                a5 + (-vv.x - v2.x) * bd + (-vv.y - v2.y) * a6,
                a5 + (vv.x - v2.x) * bd + (vv.y - v2.y) * a6,
                a5 + (vv.x + v2.x) * bd + (vv.y + v2.y) * a6,
                a5 + (-vv.x + v2.x) * bd + (-vv.y + v2.y) * a6,
            };
            vec3 col = black;
            switch (m.color) {
            case marking_color::scar: col = lerp3(underside, skin, 0.6f); break;
            case marking_color::light_skin: col = lerp3(skin, vec3{1.f, 1.f, 1.f}, 0.3f); break;
            case marking_color::dark_skin: col = skin; break;
            case marking_color::black: col = black; break;
            }
            vec4 const cs[4] = {v4(col), v4(col), v4(col), v4(col)};
            sprites::add_quad(out, marking_elements[uint32_t(m.sprite)].name, q, cs);
        }

        // the eye (and the AltVariant's shadow under it)
        {
            float const num7 = L.eye_on_body_angle * -mathf::sign(num);
            float const facing = std::cos(num + num7);
            if (facing > 0.f) {
                vec2 const p = point_on_f(L.eye_on_body_segment, num7, 0.f, false);
                vec2 const bd = body_dir_f(L.eye_on_body_segment);
                float const rot = custom::vec_to_deg(bd) - 90.f;
                float const sy = mathf::clamp01(facing * (alt ? 6.f : 2.f));
                if (alt)
                    sprites::add(out, sprite{"deerEyeA", p, rot, vec2{2.f, mathf::clamp01(facing * 4.f) * 1.25f}, vec2{0.5f, 0.5f}, skin});
                sprites::add(out, sprite{g.blink == 0 ? "deerEyeA" : "deerEyeA2", p, rot, vec2{1.f, sy}, vec2{0.5f, 0.5f}, underside});
                if (g.blink == 0) {
                    vec2 const look = g.has_look ? g.look_pos : p + bd * 120.f;
                    sprites::add(out, sprite{"deerEyeB", p + rw::clamp_magnitude((look - p) / 70.f, 1.5f), rot, vec2{1.f, sy}, vec2{0.5f, 0.5f}, black});
                }
            }
        }

        // the armour plates (the SkyWhaleCuticle shader's overlay isn't drawn)
        for (uint32_t l = 0; l < L.armor_count; ++l) {
            auto const& a = L.armor[l];
            vec2 const bd = body_dir_f(a.segment);
            constexpr uint32_t nv = 24;
            vec2 vs[nv];
            vec4 cols[nv];
            for (uint32_t m = 0; m < nv; m += 2) {
                float const num8 = float(m) / float(nv - 2) * 2.f - 1.f;
                float const num9 = mathf::clamp(num * mathf::pi / 2.f + a.angle * num8, -1.5707964f, 1.5707964f);
                float const num10 = mathf::clamp(num * mathf::pi / 2.f + (a.angle + 0.17453292f) * num8, -1.5707964f, 1.5707964f);
                vec2 const p2 = point_on_f(a.segment, num9, 0.f, true);
                vec2 const p3 = point_on_f(a.segment, num10, 0.f, true);
                vec2 const v3 = custom::perpendicular_vector(bd) * std::sin(num10);
                float const num11 = 1.f - 0.5f * std::fabs(num8);
                vs[m] = p2 + bd * a.length * (1.f - num11) * 0.2f;
                vs[m + 1] = p3 - bd * a.length * num11 + v3 * 5.f;
                vec3 const col = lerp3(lerp3(skin, vec3{1.f, 1.f, 1.f}, 0.15f), bg, mathf::pow(rw::dot(v3, vec2{0.f, 1.f}), 2.f) * 0.5f);
                cols[m] = v4(lerp3(col, skin, 0.6f));
                cols[m + 1] = v4(col);
            }
            sprites::long_mesh(out, strip, vs, cols, nv);
        }

        // containers[1]: what is in front of the body
        for (uint32_t i = 0; i < nf; ++i)
            emit(front[i]);
    }
}
