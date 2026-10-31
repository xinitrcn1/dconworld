#include <algorithm>
#include <cmath>

#include "miros_graphics.hpp"
#include "creature.hpp"

namespace miros_graphics {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using sprites::sprite;
    using miros::feather;
    using miros::graphics_state;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        // LizardScaleA0..6's sourcePixelSize.y (plumageGraphLength)
        constexpr float scale_heights[7] = {18.f, 14.f, 10.f, 18.f, 37.f, 29.f, 17.f};
        char const* scale_a(int32_t graphic) {
            static char const* const names[7] = {"LizardScaleA0", "LizardScaleA1", "LizardScaleA2", "LizardScaleA3", "LizardScaleA4", "LizardScaleA5", "LizardScaleA6"};
            return names[custom::int_clamp(graphic, 0, 6)];
        }
        // MirosBird.Head
        constexpr uint32_t head = 4;
        constexpr float head_rad = 9.f;

        // what the module reads of its bird, at a time stacker
        struct bird {
            sys::game_state const& state;
            dcon::creature_id c;
            creature::miros_payload const& p;
            miros::leg_pair const& legs;
            miros::neck_state const& neck;
            graphics_state const& g;

            bird(sys::game_state const& s, dcon::creature_id cc, graphics_state const& gs)
                : state(s), c(cc), p(s.world.creature_get_payload(cc).miros), legs(s.world.miros_data_get_legs(creature::miros_data(s.world, cc))),
                  neck(s.world.miros_data_get_neck(creature::miros_data(s.world, cc))), g(gs) {}
            vec2 pos(uint32_t i, float t) const {
                auto const k = creature::chunk(state, c, i);
                return rw::lerp(state.world.body_chunk_get_last_pos(k), state.world.body_chunk_get_pos(k), t);
            }
            float rad(uint32_t i) const { return state.world.body_chunk_get_rad(creature::chunk(state, c, i)); }
            vec2 neck_pos(uint32_t i, float t) const { return rw::lerp(neck.c[i].last_pos, neck.c[i].pos, t); }
            vec2 tip(float t) const { return neck_pos(miros::neck_chunks - 1, t); }
            float body_flip(float t) const { return mathf::lerp(p.last_body_flip, p.body_flip, t); }
            float head_flip(float t) const { return mathf::lerp(g.last_head_flip, g.head_flip, t); }
            float run_mode(float t) const {
                return (mathf::lerp(legs.l[0].last_run_mode, legs.l[0].run_mode, t) + mathf::lerp(legs.l[1].last_run_mode, legs.l[1].run_mode, t)) / 2.f;
            }
            // MirosBird.RunCycle
            float run_cycle(float spot, float t) const {
                float num = mathf::lerp(p.last_run_cycle, p.run_cycle, t) + spot;
                if (num < 0.f)
                    num += std::floor(std::fabs(num) + 1.f);
                return num - std::floor(num);
            }
            // MirosBirdGraphics.EyePos
            vec2 eye_pos(float t) const {
                float const f = head_flip(t);
                vec2 const v = pos(head, t);
                vec2 const a = custom::perpendicular_vector(custom::dir_vec(tip(t), v));
                return v + a * mathf::sign(f) * 10.f * (1.f - std::fabs(f));
            }

            //
            // NeckPlumage
            //
            vec2 neck_key_pos(int32_t i, float t) const {
                if (i == 0)
                    return pos(1, t);
                if (i >= 4) {
                    vec2 const v = pos(head, t);
                    return v + custom::dir_vec(tip(t), v) * 7.f * float(i - 3);
                }
                return neck_pos(uint32_t(i - 1), t);
            }
            float neck_key_rad(int32_t i) const {
                if (i == 0)
                    return rad(1) - 2.f;
                if (i >= 4)
                    return 6.f;
                return neck.c[i - 1].rad;
            }
            // (index 4 adds Futile.mousePosition in vanilla: it is only ever
            // weighed 0 there)
            vec2 neck_key_dir(int32_t i, float t) const {
                if (i < 4)
                    return custom::dir_vec(neck_key_pos(i, t), neck_key_pos(i + 1, t));
                return custom::dir_vec(neck_key_pos(i - 1, t), neck_key_pos(i, t));
            }
            void keys(float f, int32_t& a, int32_t& b) const {
                a = custom::int_clamp(int32_t(f * 3.f), 0, 3);
                b = custom::int_clamp(int32_t(f * 3.f + 1.f), 0, 4);
            }
            vec2 one_dimensional_neck_pos(float f, float t) const {
                int32_t a, b;
                keys(f, a, b);
                return rw::lerp(neck_key_pos(a, t), neck_key_pos(b, t), mathf::inverse_lerp(float(a), float(b), f * 3.f));
            }
            float neck_rad(float f) const {
                int32_t a, b;
                keys(f, a, b);
                return mathf::lerp(neck_key_rad(a), neck_key_rad(b), mathf::inverse_lerp(float(a), float(b), f * 3.f));
            }
            vec2 neck_dir(float f, float t) const {
                int32_t a, b;
                keys(f, a, b);
                return rw::lerp(neck_key_dir(a, t), neck_key_dir(b, t), mathf::inverse_lerp(float(a), float(b), f * 3.f));
            }
            float neck_flip(float f, float t) const { return -mathf::lerp(body_flip(t), head_flip(t), 0.5f + 0.5f * f); }
            vec2 neck_plumage_pos(float f, float t) const {
                return one_dimensional_neck_pos(f, t) + custom::perpendicular_vector(neck_dir(f, t)) * neck_flip(f, t) * neck_rad(f);
            }
            float neck_f(uint32_t i) const { return g.neck_count > 1 ? float(i) / float(g.neck_count - 1) : 0.f; }
            vec2 neck_connection(uint32_t i, float t) const { return neck_plumage_pos(neck_f(i), t); }
            vec2 neck_direction(uint32_t i) const {
                float const num = neck_f(i);
                vec2 const v = neck_dir(num, 1.f);
                if (g.neck_dir_type)
                    return rw::normalized(v * mathf::lerp(-1.f, 1.f, num) + custom::perpendicular_vector(v) * neck_flip(num, 1.f));
                return rw::normalized(custom::perpendicular_vector(v) * neck_flip(num, 1.f));
            }
            float neck_feather_flip(uint32_t i, float t) const {
                float const f2 = neck_flip(neck_f(i), t);
                return mathf::pow(std::fabs(f2), 0.2f) * mathf::sign(f2);
            }

            //
            // TailPlumage
            //
            vec2 bod_pos(float t) const { return pos(0, t); }
            vec2 bod_dir(float t) const { return custom::dir_vec(pos(1, t), bod_pos(t)); }
            float side_fac(uint32_t i, float t) const {
                float num = mathf::lerp(0.5f + body_flip(t) * 0.5f, 0.5f + 0.5f * std::sin(run_cycle(0.f, t) * mathf::pi * 2.f), run_mode(t));
                num = -1.f + 2.f * num;
                num = std::sin((g.tail_positions[i].x + num * 0.5f) * mathf::pi * 2.f);
                return num * std::sin((0.5f + g.tail_positions[i].y * 0.5f) * mathf::pi);
            }
            vec2 tail_connection(uint32_t i, float t) const {
                vec2 const b = custom::perpendicular_vector(bod_dir(t)) * side_fac(i, t) * 18.f;
                return bod_pos(t) + bod_dir(t) * 20.f * g.tail_positions[i].y + b;
            }
            vec2 tail_direction(uint32_t i) const {
                return custom::dir_vec(tail_connection(i, 1.f), bod_pos(1.f) + bod_dir(1.f) * (1.f + g.tail_positions[i].y * 2.f) * g.tail[i].length);
            }
        };

        // Plumage.Feather.Update
        void feather_update(graphics_state const& g, feather& f, vec2 dir, vec2 conn) {
            f.last_pos = f.pos;
            f.pos += f.vel;
            f.vel *= g.plumage_friction;
            f.vel.y -= g.plumage_gravity;
            f.vel += dir * g.plumage_direction * (1.f - f.lazy);
            float const num = rw::distance(f.pos, conn);
            vec2 const a = custom::dir_vec(f.pos, conn);
            f.pos -= (f.length - num) * a;
            f.vel -= (f.length - num) * a;
        }

        // Plumage.Feather's constructor
        feather make_feather(rw::unity_random& r, float length) {
            feather f;
            f.length = length;
            if (r.value() < 0.04761905f)
                f.lazy = mathf::pow(r.value(), 0.2f);
            else if (r.value() < 0.5f)
                f.lazy = mathf::pow(r.value(), 4.f);
            return f;
        }

        // BeakGraphic.OuterShape
        float outer_shape(float f) { return mathf::max(1.f - f, std::sin(f * mathf::pi)); }
    }

    void init(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        auto const d = creature::miros_data(w, c);
        auto& g = w.miros_data_get_graphics(d);
        g = graphics_state{};
        // Random.InitState(abstractCreature.ID.RandomSeed)
        rw::unity_random r{w.miros_data_get_seed(d)};
        g.plumage_graphic = r.range(0, 7);
        g.plumage_graph_length = scale_heights[custom::int_clamp(g.plumage_graphic, 0, 6)];
        g.neck_fatness = mathf::lerp(0.8f, 1.2f, r.value());
        g.beak_fatness = r.value();
        g.plumage_length = mathf::lerp(0.2f, 1.2f, mathf::pow(r.value(), 0.75f));
        float const pw = mathf::lerp(0.5f, 1.f, r.value());
        g.plumage_width = pw * (r.value() < 0.5f ? -1.f : 1.f);
        g.plumage_gravity = mathf::lerp(0.2f, 1.2f, r.value());
        g.plumage_friction = mathf::lerp(0.7f, 1.f, r.value());
        g.plumage_direction = mathf::lerp(1.f, 11.f, r.value());
        g.plumage_density = r.value();
        g.eye_size = mathf::lerp(0.5f, 1.4f, r.value());
        g.tigh_size = mathf::lerp(0.5f, 1.4f, r.value());
        // BeakGraphic
        for (int b = 0; b < 2; ++b) {
            int32_t const n = custom::int_clamp(r.range(10, 20), 0, int32_t(miros::max_teeth));
            g.teeth_count[b] = uint8_t(n);
            for (int32_t i = 0; i < n; ++i) {
                g.teeth[b][i][0] = mathf::lerp(0.1f, 0.9f, mathf::pow(r.value(), 0.8f));
                g.teeth[b][i][1] = mathf::lerp(0.5f, 1.f, r.value());
                float const v = r.value();
                g.teeth[b][i][2] = mathf::lerp(-1.f, 1.f, mathf::lerp(v, g.teeth[b][i][0], 1.f));
            }
        }
        // NeckPlumage
        if (r.value() > 0.055555556f) {
            g.has_neck_plumage = 1;
            r.value();
            float const num = mathf::lerp(6.f, 30.f, mathf::pow(g.plumage_density, 1.7f));
            g.neck_count = uint8_t(std::min<int32_t>(int32_t(num), int32_t(miros::max_feathers)));
            int32_t const kind = r.range(0, 3);
            for (uint32_t i = 0; i < g.neck_count; ++i) {
                float const num3 = g.neck_count > 1 ? float(i) / float(g.neck_count - 1) : 0.f;
                float num4 = 1.f;
                if (kind == 0)
                    num4 = r.value();
                else if (kind == 1)
                    num4 = std::sin(num3 * mathf::pi);
                else if (kind == 2)
                    num4 = num3 == 1.f ? 0.65f : num3;
                float const len = g.plumage_graph_length * g.plumage_length * num4 * mathf::lerp(0.9f, 1.1f, r.value());
                g.neck[i] = make_feather(r, len);
            }
            g.neck_dir_type = r.value() < 0.5f ? 1 : 0;
        }
        // TailPlumage
        if (r.value() > 0.055555556f) {
            g.has_tail_plumage = 1;
            g.tail_count = uint8_t(std::min<int32_t>(int32_t(mathf::lerp(2.f, 30.f, g.plumage_density)), int32_t(miros::max_feathers)));
            float num2 = mathf::lerp(1.f, 3.f, mathf::pow(r.value(), 4.f));
            if (r.value() < 0.05f)
                num2 = r.value();
            for (uint32_t i = 0; i < g.tail_count; ++i) {
                float const px = -1.f + 2.f * r.value();
                float const py = r.value();
                g.tail_positions[i] = vec2{px, py};
                g.tail_flips[i] = r.value() < 0.5f ? -1.f : 1.f;
                // (case 3: the length by the feather's height on the body)
                float const len = g.plumage_graph_length * g.plumage_length * py * mathf::lerp(0.9f, 1.1f, r.value()) * num2;
                g.tail[i] = make_feather(r, len);
                g.tail[i].lazy = mathf::lerp(g.tail[i].lazy, 1.f, 0.5f);
            }
        }
        g.eye_col = custom::hsl2rgb(mathf::lerp(0.08f, 0.17f, r.value()), 1.f, 0.5f);
        reset_positions(state, c);
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        auto& g = w.miros_data_get_graphics(creature::miros_data(w, c));
        bird const b{state, c, g};
        // Plumage.Reset
        for (uint32_t i = 0; i < g.neck_count; ++i) {
            g.neck[i].pos = g.neck[i].last_pos = b.neck_connection(i, 1.f);
            g.neck[i].vel = vec2{0.f, 0.f};
        }
        for (uint32_t i = 0; i < g.tail_count; ++i) {
            g.tail[i].pos = g.tail[i].last_pos = b.tail_connection(i, 1.f);
            g.tail[i].vel = vec2{0.f, 0.f};
        }
        // EyeTrail.Reset
        g.eye_trail[0] = b.pos(head, 1.f);
        g.eye_trail_count = 1;
    }

    void update(sys::game_state& state, dcon::creature_id c, world::room_view const&) {
        auto& w = state.world;
        auto& g = w.miros_data_get_graphics(creature::miros_data(w, c));
        bird const b{state, c, g};
        g.last_head_flip = g.head_flip;
        if (custom::distance_to_line(b.pos(head, 1.f), b.pos(1, 1.f), b.pos(0, 1.f)) < 0.f)
            g.head_flip = mathf::min(1.f, g.head_flip + 0.16666667f);
        else
            g.head_flip = mathf::max(-1.f, g.head_flip - 0.16666667f);
        // Plumage.Update
        for (uint32_t i = 0; i < g.neck_count; ++i)
            feather_update(g, g.neck[i], b.neck_direction(i), b.neck_connection(i, 1.f));
        for (uint32_t i = 0; i < g.tail_count; ++i)
            feather_update(g, g.tail[i], b.tail_direction(i), b.tail_connection(i, 1.f));
        // EyeTrail.Update
        for (uint32_t i = miros::eye_trail_positions - 1; i > 0; --i)
            g.eye_trail[i] = g.eye_trail[i - 1];
        g.eye_trail[0] = b.eye_pos(1.f);
        g.eye_trail_count = uint8_t(std::min<uint32_t>(g.eye_trail_count + 1u, miros::eye_trail_positions));
        // (the feet's LightSources aren't ported)
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip) {
        auto const& w = state.world;
        auto const& g = w.miros_data_get_graphics(creature::miros_data(w, c));
        bird const b{state, c, g};
        float const t = alpha;
        // ApplyPalette: every sprite but the eye trail the palette's black
        vec3 const black = pal.black();
        vec3 const eye = g.eye_col;

        float const num = b.head_flip(t);
        vec2 const hp = b.pos(head, t);
        vec2 const hd = custom::dir_vec(b.tip(t), hp);
        vec2 const hperp = custom::perpendicular_vector(hd);
        float const hang = custom::vec_to_deg(hd);
        vec2 const body = b.pos(0, t);
        vec2 const body_a = custom::dir_vec(body, b.pos(1, t));

        // NeckSprite: TriangleMesh.MakeLongMesh(3)
        vec2 neck[12];
        {
            vec2 v5 = b.pos(1, t);
            float num3 = 8.f;
            for (uint32_t l = 0; l < miros::neck_chunks; ++l) {
                vec2 v6 = b.neck_pos(l, t);
                if (l == miros::neck_chunks - 1)
                    v6 = rw::lerp(v6, hp, 0.5f);
                else if (l == 0)
                    v6 = rw::lerp(v6, body + body_a * 40.f, 0.3f);
                vec2 const nd = rw::normalized(v6 - v5);
                vec2 const a2 = custom::perpendicular_vector(nd);
                float const dd = rw::distance(v6, v5) / 5.f;
                float const r = b.neck.c[l].rad;
                neck[l * 4] = v5 - a2 * (r + num3) * 0.5f * g.neck_fatness + nd * dd * (l == 0 ? 0.f : 1.f);
                neck[l * 4 + 1] = v5 + a2 * (r + num3) * 0.5f * g.neck_fatness + nd * dd * (l == 0 ? 0.f : 1.f);
                neck[l * 4 + 2] = v6 - a2 * r * g.neck_fatness - nd * dd * (l == miros::neck_chunks - 1 ? 0.f : 1.f);
                neck[l * 4 + 3] = v6 + a2 * r * g.neck_fatness - nd * dd * (l == miros::neck_chunks - 1 ? 0.f : 1.f);
                num3 = r;
                v5 = v6;
            }
        }
        sprites::long_mesh(out, strip, neck, 12, black);

        // BodySprite
        {
            vec2 const h0 = rw::lerp(b.legs.l[0].joints[0].last_pos, b.legs.l[0].joints[0].pos, t);
            vec2 const h1 = rw::lerp(b.legs.l[1].joints[0].last_pos, b.legs.l[1].joints[0].pos, t);
            sprites::add(out, sprite{"MirosBody", body, custom::aim(body, b.pos(1, t)), vec2{custom::lerp_map(rw::distance(h0, h1), 5.f, 50.f, 0.75f, 1.1f), 1.f},
                vec2{0.5f, 0.4f}, black});
        }

        // LegGraphic: thigh, piston, shin, its small part, foot
        for (auto const& l : b.legs.l) {
            vec2 v2 = rw::lerp(l.joints[0].last_pos, l.joints[0].pos, t);
            if (!custom::dist_less(v2, body, 20.f))
                v2 = body + custom::dir_vec(body, v2) * 20.f;
            vec2 v3 = rw::lerp(l.joints[1].last_pos, l.joints[1].pos, t);
            vec2 const v4 = rw::lerp(l.joints[2].last_pos, l.joints[2].pos, t);
            float const s = -mathf::sign(l.flip);
            sprites::add(out, sprite{"MirosTigh", v2, custom::aim(v2, v3) + 90.f, vec2{(rw::distance(v2, v3) + 5.f) / 44.f, s * g.tigh_size}, vec2{0.9f, 0.5f}, black});
            vec2 const v5 = custom::dir_vec(v3, v4);
            vec2 const a = custom::perpendicular_vector(v5) * l.flip;
            v3 += a * 5.f;
            float const rot = custom::aim(v4, v3);
            sprites::add(out, sprite{"pixel", v4, rot, vec2{3.f, 70.f}, vec2{0.5f, 0.f}, black});
            sprites::add(out, sprite{"MirosLeg", v3 - a * 7.f, rot, vec2{s, 1.f}, vec2{0.6f, 0.5f}, black});
            sprites::add(out, sprite{"MirosLegSmallPart", v3 - a * 8.f + v5 * 18.f, rot, vec2{s, 1.f}, vec2{0.6f, 0.5f}, black});
            sprites::add(out, sprite{"deerEyeB", v4, 0.f, vec2{1.f, 1.f}, vec2{0.5f, 0.5f}, black});
        }

        // BeakGraphic: the half's mesh, then its teeth
        float const jaw = mathf::lerp(b.p.last_jaw_open, b.p.jaw_open, t);
        for (int index = 0; index < 2; ++index) {
            float const side = -1.f + 2.f * float(index);
            vec2 const v = custom::deg_to_vec(hang + 60.f * jaw * side * num * mathf::pow(std::fabs(num), 0.5f));
            vec2 const a = custom::perpendicular_vector(v) * side * mathf::sign(num);
            vec2 m[13];
            m[0] = hp + hd * head_rad;
            m[1] = hp + hperp * (1.f - 2.f * float(index)) * head_rad * num;
            float const dd = 65.f * mathf::lerp(mathf::lerp(1.f, 0.6f, jaw), 1.f, std::fabs(num));
            float const d2 = mathf::lerp(5.f, 7.f, jaw) * mathf::lerp(0.5f, 1.5f, g.beak_fatness);
            for (int i = 1; i < 6; ++i) {
                float const f = float(i) / 6.f;
                vec2 const a2 = hp + hd * head_rad * (1.f - f) * jaw + v * dd * f;
                m[i * 2] = a2 + a * d2 * mathf::lerp(0.6f, 1.f, jaw) * outer_shape(f) * mathf::pow(1.f - std::fabs(num), 0.4f);
                m[i * 2 + 1] = a2 - a * d2 * outer_shape(f) * std::fabs(num);
            }
            m[12] = hp + v * dd + a * d2 * 0.75f * jaw;
            sprites::long_mesh(out, strip, m, 13, black);
            for (uint32_t j = 0; j < g.teeth_count[index]; ++j) {
                float const* tt = g.teeth[index][j];
                vec2 const v2 = hp + hd * head_rad * (1.f - tt[0]) * jaw + v * dd * tt[0];
                float const open = mathf::inverse_lerp(0.f, 0.1f, jaw);
                sprites::add(out, sprite{"LizardScaleA6", v2, custom::vec_to_deg(rw::normalized(-a - v * tt[2])),
                    vec2{0.4f * open * tt[1], 0.5f * outer_shape(tt[0]) * std::fabs(num) * open * tt[1]}, vec2{0.5f, 0.8f}, black});
            }
        }

        // HeadSprite
        sprites::add(out, sprite{"Circle20", hp, hang, vec2{0.9f, 1.2f}, vec2{0.5f, 0.5f}, black});

        // the plumage: the neck's feathers, then the tail's
        char const* const scale = scale_a(g.plumage_graphic);
        for (uint32_t i = 0; i < g.neck_count; ++i) {
            vec2 const v = b.neck_connection(i, t);
            auto const& f = g.neck[i];
            sprites::add(out, sprite{scale, v, custom::aim(v, rw::lerp(f.last_pos, f.pos, t)), vec2{g.plumage_width * b.neck_feather_flip(i, t), f.length / g.plumage_graph_length},
                vec2{0.5f, 0.f}, black});
        }
        for (uint32_t i = 0; i < g.tail_count; ++i) {
            vec2 const v = b.tail_connection(i, t);
            auto const& f = g.tail[i];
            sprites::add(out, sprite{scale, v, custom::aim(v, rw::lerp(f.last_pos, f.pos, t)), vec2{g.plumage_width * g.tail_flips[i], f.length / g.plumage_graph_length},
                vec2{0.5f, 0.f}, black});
        }

        // EyeSprite
        sprites::add(out, sprite{"Circle20", b.eye_pos(t), hang, vec2{0.3f * std::fabs(num) * g.eye_size, 0.3f * g.eye_size}, vec2{0.5f, 0.5f}, eye});

        // EyeTrail (in the Water container, over the rest): MakeLongMesh(9)
        // fading out behind the eye, seen in the dark
        {
            constexpr uint32_t segs = miros::eye_trail_positions - 1;
            uint32_t const count = std::max<uint32_t>(g.eye_trail_count, 1u);
            auto const get = [&](int32_t i) { return g.eye_trail[custom::int_clamp(i, 0, int32_t(count) - 1)]; };
            auto const smooth = [&](int32_t i) { return rw::lerp(get(i + 1), get(i), t); };
            vec2 v[segs * 4];
            vec4 col[segs * 4];
            vec2 a = b.eye_pos(t);
            float const dd = 2.f * g.eye_size;
            for (uint32_t i = 0; i < segs; ++i) {
                vec2 const sp = smooth(int32_t(i));
                vec2 const sp2 = smooth(int32_t(i) + 1);
                vec2 vec = rw::normalized(a - sp);
                vec2 const a2 = custom::perpendicular_vector(vec);
                vec *= rw::distance(a, sp2) / 5.f;
                v[i * 4] = a - a2 * dd - vec;
                v[i * 4 + 1] = a + a2 * dd - vec;
                v[i * 4 + 2] = sp - a2 * dd + vec;
                v[i * 4 + 3] = sp + a2 * dd + vec;
                a = sp;
            }
            // (Room.Darkness: the palette's darkness, without the light sources)
            float const dark = mathf::inverse_lerp(0.4f, 0.6f, pal.darkness());
            for (uint32_t j = 0; j < segs * 4; ++j) {
                float const f = float(j) / float(segs * 4 - 1);
                col[j] = vec4{eye.x, eye.y, eye.z, mathf::pow(1.f - f, 2.f) * 0.25f * dark};
            }
            if (dark > 0.f)
                sprites::long_mesh(out, strip, v, col, segs * 4);
        }
    }
}
