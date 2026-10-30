#include <cmath>

#include "tube_worm_graphics.hpp"
#include "body_parts.hpp"
#include "creature.hpp"
#include "rot.hpp"
#include "tube_worm.hpp"

namespace tube_worm_graphics {
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using world::room_view;
    using tube_worm::tongue_mode;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        // GenericBodyPart(rad 5, surface friction 0.5, air friction 0.99)
        constexpr float part_rad = 5.f, part_surface_friction = 0.5f, part_air_friction = 0.99f;
        constexpr uint32_t n_parts = tube_worm::body_parts;
        constexpr uint32_t n_segs = tube_worm::rope_segments;

        // Color.Lerp
        vec3 lerp(vec3 a, vec3 b, float t) {
            t = mathf::clamp01(t);
            return a + (b - a) * t;
        }

        tube_worm::graphics_state& graphics(sys::game_state& state, dcon::creature_id c) {
            return state.world.tube_worm_data_get_graphics(creature::tube_worm_data(state.world, c));
        }
        tube_worm::graphics_state const& graphics(sys::game_state const& state, dcon::creature_id c) {
            return state.world.tube_worm_data_get_graphics(creature::tube_worm_data(state.world, c));
        }
        vec2 chunk_pos(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_pos(creature::chunk(state, c, i));
        }

        // Rope.TotalPositions, Rope.GetPosition
        uint32_t total_positions(world::rope_state const& r) { return 2u + r.bend_count; }
        vec2 rope_position(world::rope_state const& r, uint32_t i) {
            if (i == 0)
                return r.a;
            if (i - 1 >= r.bend_count)
                return r.b;
            return r.bends[i - 1].pos;
        }

        // RopeSegment.Update: an unclaimed segment drifts to the line between
        // the claimed ones either side of it
        void segment_update(tube_worm::graphics_state& g, uint32_t index, bool both_in) {
            auto& s = g.rope[index];
            if (s.claimed_for_bend)
                return;
            s.last_pos = s.pos;
            s.pos += s.vel;
            s.vel *= 0.98f;
            uint32_t i = index, j = index;
            while (i > 0) {
                --i;
                if (g.rope[i].claimed_for_bend)
                    break;
            }
            while (j < n_segs - 1) {
                ++j;
                if (g.rope[j].claimed_for_bend)
                    break;
            }
            vec2 const v = rw::lerp(g.rope[i].pos, g.rope[j].pos, mathf::inverse_lerp(float(i), float(j), float(index)));
            if (both_in) {
                s.pos = v;
                return;
            }
            s.vel += (v - s.pos) * 0.2f;
            s.pos = rw::lerp(s.pos, v, 0.4f);
        }

        // ConnectRopeSegments
        void connect_segments(tube_worm::graphics_state& g, uint32_t a, uint32_t b, float rope_lengths) {
            auto& sa = g.rope[a];
            auto& sb = g.rope[b];
            vec2 const dir = custom::dir_vec(sa.pos, sb.pos);
            float const dist = rw::distance(sa.pos, sb.pos);
            float const want = rope_lengths / float(n_segs) * 0.1f;
            if (!sa.claimed_for_bend) {
                sa.pos += dir * (dist - want) * 0.5f;
                sa.vel += dir * (dist - want) * 0.5f;
            }
            if (!sb.claimed_for_bend) {
                sb.pos -= dir * (dist - want) * 0.5f;
                sb.vel -= dir * (dist - want) * 0.5f;
            }
        }

        // AlignRope: the segment at `f` along the rope pinned to `at`
        void align_rope(tube_worm::graphics_state& g, float f, vec2 at) {
            auto& s = g.rope[uint32_t(custom::int_clamp(int32_t(f * float(n_segs)), 0, int32_t(n_segs) - 1))];
            s.last_pos = s.pos;
            s.pos = at;
            s.vel = vec2{0.f, 0.f};
            s.claimed_for_bend = 1;
        }

        // the root of a tongue in use pokes out along it; one pulled in
        // trails behind its chunk
        void tongue_root(sys::game_state const& state, dcon::creature_id c, tube_worm::graphics_state& g, uint32_t part, uint32_t next, uint32_t tongue,
            float total) {
            constexpr float len = 10.f;
            auto const& t = tube_worm::tongue_of(state, c, tongue);
            vec2 const own = chunk_pos(state, c, tongue);
            vec2 const other = chunk_pos(state, c, 1 - tongue);
            auto& b = g.body[part];
            if (t.mode != tongue_mode::retracted) {
                b.pos = own + custom::dir_vec(own, rot::rope_a_connect(t.rope)) * len * custom::lerp_map(total, 10.f, 500.f, 0.7f, 1.7f);
                b.vel = b.last_pos - b.pos;
                return;
            }
            vec2 const a = custom::dir_vec(b.pos, g.body[next].pos);
            float const d = rw::distance(b.pos, g.body[next].pos);
            b.pos += a * (d - len) * 0.5f;
            b.vel += a * (d - len) * 0.5f;
            b.vel += custom::dir_vec(other, own);
        }

        // SegmentStretchFac: thinner where the body is pulled long
        float segment_stretch_fac(tube_worm::graphics_state const& g, uint32_t seg, float alpha) {
            auto const at = [&](uint32_t i) { return rw::lerp(g.body[i].last_pos, g.body[i].pos, alpha); };
            float n = 0.f;
            if (seg > 0)
                n += 10.f / rw::distance(at(seg), at(seg - 1));
            if (seg < n_parts - 1)
                n += 10.f / rw::distance(at(seg), at(seg + 1));
            if (seg > 0 && seg < n_parts - 1)
                n /= 2.f;
            if (n < 1.f)
                return mathf::pow(n, 0.6f);
            return custom::lerp_map(n, 1.f, 3.f, 1.f, 1.5f);
        }
    }

    void reset(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        auto& g = graphics(state, c);
        g = tube_worm::graphics_state{};
        w.creature_get_graphics_random(c) = rw::rng{w.creature_get_random(c).state ^ 0x7B3E4u};
        // the colour, from the creature's seed (Random.InitState(ID.RandomSeed))
        rw::unity_random r{w.tube_worm_data_get_seed(creature::tube_worm_data(w, c))};
        float const h = mathf::lerp(0.52f, 0.68f, r.value());
        float const s = mathf::lerp(0.4f, 0.9f, r.value());
        float const l = mathf::lerp(0.15f, 0.3f, r.value());
        g.color = custom::hsl2rgb(h, s, l);
        for (auto& b : g.body)
            b.rad = part_rad;
        reset_positions(state, c);
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        auto& g = graphics(state, c);
        auto& rng = state.world.creature_get_graphics_random(c);
        vec2 const at = chunk_pos(state, c, 0);
        // BodyPart.Reset(firstChunk.pos)
        for (auto& b : g.body) {
            b.pos = at + custom::deg_to_vec(rng.value() * 360.f);
            b.last_pos = b.pos;
            b.vel = vec2{0.f, 0.f};
        }
        for (auto& s : g.rope) {
            s.pos = s.last_pos = at;
            s.vel = vec2{0.f, 0.f};
            s.claimed_for_bend = 0;
        }
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto& g = graphics(state, c);
        auto const& x = w.creature_get_payload(c).tube_worm;
        auto const& t0 = tube_worm::tongue_of(state, c, 0);
        auto const& t1 = tube_worm::tongue_of(state, c, 1);
        g.last_stretch = g.stretch;
        g.stretch = tube_worm::rope_stretch_fac(state, c);
        for (auto& b : g.body)
            body_parts::generic_update(b, room, part_air_friction, part_surface_friction);
        // moveLoop: louder the faster it inches along
        state.play_loop(sound::loop_key(sound::loop_owner::creature, uint32_t(c.index())), sound::sound_id::tube_worm_inch_along_tongue_loop, room.id,
            chunk_pos(state, c, 0), mathf::inverse_lerp(0.f, 0.029f, std::fabs(x.last_walk - x.walk_cycle)));
        for (uint32_t i = 0; i < 2; ++i) {
            auto const k = creature::chunk(state, c, i);
            g.body[1 + i].pos = w.body_chunk_get_pos(k);
            g.body[1 + i].vel = w.body_chunk_get_vel(k);
        }
        float const total = tube_worm::total_rope(state, c);
        tongue_root(state, c, g, 0, 1, 0, total);
        tongue_root(state, c, g, 3, 2, 1, total);
        vec2 const c0 = chunk_pos(state, c, 0), c1 = chunk_pos(state, c, 1);
        if (custom::dist_less(c1, g.body[0].pos, 14.f))
            g.body[0].pos = c1 + custom::dir_vec(c1, g.body[0].pos) * 14.f;
        if (custom::dist_less(c0, g.body[3].pos, 14.f))
            g.body[3].pos = c0 + custom::dir_vec(c0, g.body[3].pos) * 14.f;

        // the points the rope runs through: the front tongue's rope from its
        // tip in, the body, the back tongue's rope out to its tip
        vec2 points[2 * (world::rope_state::max_bends + 2) + n_parts];
        uint32_t n = 0;
        bool const out0 = t0.mode != tongue_mode::retracted, out1 = t1.mode != tongue_mode::retracted;
        if (out0) {
            for (uint32_t j = total_positions(t0.rope) - 1; j > 0; --j)
                points[n++] = rope_position(t0.rope, j);
            points[n++] = g.body[0].pos;
        }
        if (out0 == out1)
            for (uint32_t k = 1; k < 3; ++k)
                points[n++] = g.body[k].pos;
        if (out1) {
            points[n++] = g.body[3].pos;
            for (uint32_t l = 1; l < total_positions(t1.rope); ++l)
                points[n++] = rope_position(t1.rope, l);
        }
        float length = 0.f;
        for (uint32_t m = 1; m < n; ++m)
            length += rw::distance(points[m - 1], points[m]);
        float along = 0.f;
        for (uint32_t m = 0; m < n; ++m) {
            if (m > 0)
                along += rw::distance(points[m - 1], points[m]);
            align_rope(g, length > 0.f ? along / length : 0.f, points[m]);
        }
        bool const both_in = !out0 && !out1;
        for (uint32_t k = 0; k < n_segs; ++k)
            segment_update(g, k, both_in);
        float const ropes = t0.rope.total_length + t1.rope.total_length;
        for (uint32_t k = 1; k < n_segs; ++k)
            connect_segments(g, k, k - 1, ropes);
        for (auto& s : g.rope)
            s.claimed_for_bend = 0;
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, palette const& pal, bool tips, std::vector<sprites::draw_cmd>& out) {
        auto const& g = graphics(state, c);
        auto const part = [&](uint32_t i) { return rw::lerp(g.body[i].last_pos, g.body[i].pos, alpha); };
        auto const seg = [&](uint32_t i) { return rw::lerp(g.rope[i].last_pos, g.rope[i].pos, alpha); };

        // sprites 3 and 4 (drawn first: the container takes them in reverse):
        // a sliver of Circle20 at each end of the rope
        vec3 const tip_color = lerp(pal.fog, custom::hsl2rgb(0.95f, 1.f, 0.865f), 0.7f);
        if (tips)
            for (uint32_t k : {n_segs - 1, 0u}) {
                vec2 const a = seg(k);
                vec2 const b = seg(k == 0 ? 1 : n_segs - 2);
                sprites::sprite s;
                s.element = "Circle20";
                s.pos = a;
                s.scale = vec2{0.2f, (rw::distance(a, b) + 1.f) / 20.f};
                s.anchor = vec2{0.5f, 0.f};
                s.rotation = custom::aim(a, b);
                s.color = tip_color;
                sprites::add(out, s);
            }

        // sprite 2: the rope, thicker in the middle where it is slack, pink
        // fading into the fog at its ends
        float const stretch = mathf::lerp(g.last_stretch, g.stretch, alpha);
        constexpr uint32_t rope_verts = (n_segs - 1) * 4;
        vec2 rv[rope_verts];
        vec4 rc[rope_verts];
        vec2 at = seg(1);
        at += custom::dir_vec(seg(2), at) * 1.f;
        for (uint32_t k = 1; k < n_segs; ++k) {
            float const f = float(k) / float(n_segs - 1);
            vec2 const p = seg(k);
            vec2 const side = custom::perpendicular_vector(rw::normalized(at - p));
            float const w = 0.2f + 1.6f * mathf::lerp(1.f, stretch, mathf::pow(std::sin(f * mathf::pi), 0.7f));
            rv[(k - 1) * 4] = at - side * w;
            rv[(k - 1) * 4 + 1] = at + side * w;
            rv[(k - 1) * 4 + 2] = p - side * w;
            rv[(k - 1) * 4 + 3] = p + side * w;
            at = p;
        }
        for (uint32_t j = 0; j < rope_verts; ++j) {
            float const f = mathf::clamp01(std::sin(float(j) / float(rope_verts - 1) * mathf::pi));
            vec3 const col = lerp(pal.fog, custom::hsl2rgb(mathf::lerp(0.95f, 1.f, f), 1.f, mathf::lerp(0.75f, 0.9f, mathf::pow(f, 0.15f))), 0.7f);
            rc[j] = vec4{col.x, col.y, col.z, 1.f};
        }
        sprites::long_mesh(out, rv, rc, rope_verts);

        // sprites 1 and 0: the body, the palette's black under its colour
        vec2 bv[n_parts * 4];
        vec2 v = part(0);
        v += custom::dir_vec(part(1), v) * 10.f;
        for (uint32_t i = 0; i < n_parts; ++i) {
            vec2 const p = part(i);
            vec2 const next = i < n_parts - 1 ? part(i + 1) : p + custom::dir_vec(v, p) * 10.f;
            vec2 const dir = rw::normalized(v - p);
            vec2 const side = custom::perpendicular_vector(dir);
            float const d = rw::distance(p, v) / 4.f;
            float const d2 = rw::distance(p, next) / 4.f;
            float const s = segment_stretch_fac(g, i, alpha);
            float w0 = 5.f * s, w1 = 5.f * s;
            if (i == 0)
                w0 = 3.5f * s;
            else if (i == n_parts - 1)
                w1 = 3.5f * s;
            bv[i * 4] = v - side * w0 - dir * d;
            bv[i * 4 + 1] = v + side * w0 - dir * d;
            bv[i * 4 + 2] = p - side * w1 + dir * d2;
            bv[i * 4 + 3] = p + side * w1 + dir * d2;
            v = p;
        }
        sprites::long_mesh(out, bv, n_parts * 4, pal.black);
        sprites::long_mesh(out, bv, n_parts * 4, g.color);
    }
}
