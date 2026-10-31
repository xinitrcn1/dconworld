#include <algorithm>
#include <bit>
#include <cmath>
#include <string_view>

#include "barnacle_graphics.hpp"
#include "barnacle.hpp"
#include "body_parts.hpp"
#include "creature.hpp"
#include "physics.hpp"
#include "rooms.hpp"
#include "tracker.hpp"

namespace barnacle_graphics {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using world::room_view;
    using sprites::sprite;
    using barnacle::cone;
    using barnacle::graphics_state;
    using barnacle::inner_rad;
    using barnacle::leg_count;
    using barnacle::leg_length;
    using barnacle::outer_rad;
    using player::limb_mode;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        // Limb(owner, chunk 0, i, rad 1, surface friction 0.5, air friction
        // 0.9, huntSpeed 7, quickness 0.8)
        constexpr body_parts::limb_params leg_params{1.f, 0.5f, 0.9f, 7.f, 0.8f};

        graphics_state& gs(sys::game_state& state, dcon::creature_id c) {
            return state.world.barnacle_data_get_graphics(creature::barnacle_data(state.world, c));
        }
        graphics_state const& gs(sys::game_state const& state, dcon::creature_id c) {
            return state.world.barnacle_data_get_graphics(creature::barnacle_data(state.world, c));
        }
        barnacle::creature_params const& prm(sys::game_state const& state, dcon::creature_id c) {
            return state.world.barnacle_data_get_params(creature::barnacle_data(state.world, c));
        }
        creature::barnacle_payload const& pl(sys::game_state const& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).barnacle;
        }
        dcon::body_chunk_id ch(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return barnacle::chunk(state, c, i);
        }
        vec2 at_time(sys::game_state const& state, dcon::creature_id c, uint32_t i, float t) {
            auto const k = ch(state, c, i);
            return rw::lerp(state.world.body_chunk_get_last_pos(k), state.world.body_chunk_get_pos(k), t);
        }
        vec3 lerp3(vec3 a, vec3 b, float t) {
            t = mathf::clamp01(t);
            return a + (b - a) * t;
        }
        float magnitude3(vec3 v) {
            return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        }
        vec3 normalized3(vec3 v) {
            float const m = magnitude3(v);
            return m > 1e-5f ? v / m : vec3{0.f, 0.f, 0.f};
        }
        // Vector3.Slerp: the direction turned and the length lerped
        vec3 slerp3(vec3 a, vec3 b, float t) {
            t = mathf::clamp01(t);
            float const la = magnitude3(a), lb = magnitude3(b);
            vec3 const na = normalized3(a), nb = normalized3(b);
            float const d = mathf::clamp(na.x * nb.x + na.y * nb.y + na.z * nb.z, -1.f, 1.f);
            float const len = mathf::lerp(la, lb, t);
            if (d > 0.9995f || la < 1e-5f || lb < 1e-5f)
                return normalized3(na + (nb - na) * t) * len;
            float const theta = std::acos(d) * t;
            vec3 const rel = normalized3(nb - na * d);
            return (na * std::cos(theta) + rel * std::sin(theta)) * len;
        }
        // Quaternion.AngleAxis(a, forward) * Quaternion.AngleAxis(b, right):
        // about x by b, then about z by a (degrees)
        struct rotation {
            float ca, sa, cb, sb;
            vec3 operator*(vec3 v) const {
                float const y = v.y * cb - v.z * sb;
                float const z = v.y * sb + v.z * cb;
                return vec3{v.x * ca - y * sa, v.x * sa + y * ca, z};
            }
        };
        rotation make_rotation(float a_deg, float b_deg) {
            float const a = a_deg * mathf::deg2rad, b = b_deg * mathf::deg2rad;
            return rotation{std::cos(a), std::sin(a), std::cos(b), std::sin(b)};
        }
        vec2 xy(vec3 v) {
            return vec2{v.x, v.y};
        }

        // GetShellBottom(timeStacker)
        vec2 shell_bottom(sys::game_state const& state, dcon::creature_id c, graphics_state const& g, float t) {
            float const num = mathf::lerp(g.last_roll, g.roll, t);
            vec2 const a = at_time(state, c, 0, t), b = at_time(state, c, 1, t);
            vec2 const a2 = custom::perpendicular_vector(rw::normalized(a - b));
            return a + a2 * state.world.body_chunk_get_rad(ch(state, c, 0)) * 0.2f * std::sin(num * mathf::pi / 2.f);
        }
        // GetLegAttachment(index, timeStacker)
        vec2 leg_attachment(sys::game_state const& state, dcon::creature_id c, graphics_state const& g, uint32_t index, float t) {
            float const num = index % 2 == 0 ? 1.f : -1.f;
            float const d = float(index / 2) / 2.f;
            float const num2 = mathf::lerp(g.last_roll, g.roll, t);
            vec2 const a = at_time(state, c, 0, t), b = at_time(state, c, 1, t);
            vec2 const n = rw::normalized(a - b);
            vec2 const a2 = custom::perpendicular_vector(n);
            float const rad = state.world.body_chunk_get_rad(ch(state, c, 0));
            return shell_bottom(state, c, g, t) + n * rad * 0.5f * d + a2 * rad * 0.15f * std::sin(num * mathf::pi * 0.25f + num2 * mathf::pi * 0.5f);
        }
        // GetLegTarget(index)
        vec2 leg_target(sys::game_state const& state, dcon::creature_id c, graphics_state const& g, uint32_t index) {
            float const num = index % 2 == 0 ? 1.f : -1.f;
            float const d = float(index / 2) / 2.f;
            vec2 const n = rw::normalized(state.world.body_chunk_get_pos(ch(state, c, 0)) - state.world.body_chunk_get_pos(ch(state, c, 1)));
            vec2 const a = custom::perpendicular_vector(n);
            return leg_attachment(state, c, g, index, 1.f) + n * leg_length * d + a * leg_length * std::sin(num * mathf::pi * 0.25f + g.roll * mathf::pi * 0.5f);
        }

        // GenerateCones (BarnacleGraphics' constructor, Random from the
        // ID's seed again): cones over the upper half of the shell's
        // sphere, pushed apart, drawn up, their tips out to a random length
        void generate_cones(graphics_state& g, barnacle::creature_params const& cp, int32_t seed) {
            rw::unity_random r{seed};
            uint32_t const n = uint32_t(std::clamp<int32_t>(cp.cones, 0, int32_t(barnacle::max_cones)));
            g.cone_count = uint8_t(n);
            for (uint32_t i = 0; i < n; ++i) {
                vec3 v = r.on_unit_sphere();
                v.y = std::fabs(v.y);
                float num = 1.f + r.range(-0.3f, 0.3f) * cp.cone_rad_variance;
                num /= float(n) / 15.f;
                float const num2 = 6.5f * num;
                cone k;
                k.base_pos = v * inner_rad;
                k.tip_pos = vec3{0.f, 0.f, 0.f};
                k.base_rad = num2;
                k.tip_rad = num2 * cp.tip_rad_factor;
                k.eye_rad = num2 * cp.tip_rad_factor * cp.eye_size_multiplier * r.range(0.5f, 0.9f);
                k.lightness = mathf::lerp(0.5f, r.value(), cp.cone_color_variance);
                g.cones[i] = k;
            }
            for (int j = 0; j < 50; ++j) {
                for (uint32_t k = 0; k < n; ++k)
                    for (uint32_t l = k + 1; l < n; ++l) {
                        float const num4 = g.cones[k].base_rad + g.cones[l].base_rad;
                        vec3 const a = g.cones[l].base_pos - g.cones[k].base_pos;
                        float const mag = magnitude3(a);
                        if (mag < num4 && mag > 0.f) {
                            vec3 const a2 = a / mag * (num4 - mag);
                            g.cones[k].base_pos -= a2 * 0.2f;
                            g.cones[l].base_pos += a2 * 0.2f;
                            g.cones[k].base_pos = normalized3(g.cones[k].base_pos) * inner_rad;
                            g.cones[l].base_pos = normalized3(g.cones[l].base_pos) * inner_rad;
                        }
                    }
                for (uint32_t m = 0; m < n; ++m) {
                    g.cones[m].base_pos = slerp3(g.cones[m].base_pos, vec3{0.f, 1.f, 0.f}, 0.1f);
                    if (g.cones[m].base_pos.y < 0.f)
                        g.cones[m].base_pos.y = 0.f;
                }
            }
            for (uint32_t i = 0; i < n; ++i)
                g.cones[i].tip_pos = normalized3(g.cones[i].base_pos)
                    * mathf::lerp(inner_rad, outer_rad, mathf::lerp(0.5f, r.range(0.2f, 1.f), cp.cone_length_variance));
        }

        // a JaggedCircle shaded Futile_White (16 pixels): a ragged ellipse of
        // the sprite's scale, turned like a sprite (clockwise degrees)
        void jagged_ellipse(std::vector<sprites::draw_cmd>& out, vec2 pos, vec2 scale, float rot, uint32_t seed, vec3 color) {
            constexpr int segs = 18;
            float radii[segs];
            uint32_t h = seed | 1u;
            for (int k = 0; k < segs; ++k) {
                h ^= h << 13;
                h ^= h >> 17;
                h ^= h << 5;
                radii[k] = 0.84f + 0.16f * float(h & 0xFFFF) / 65535.f;
            }
            vec2 const up = custom::deg_to_vec(rot);
            vec2 const right = custom::deg_to_vec(rot + 90.f);
            auto const corner = [&](int k) {
                float const ang = float(k % segs) * 2.f * mathf::pi / float(segs);
                float const r = radii[k % segs];
                return pos + right * (std::cos(ang) * 8.f * scale.x * r) + up * (std::sin(ang) * 8.f * scale.y * r);
            };
            for (int k = 0; k < segs; ++k) {
                sprites::draw_cmd cmd;
                cmd.is_triangle = true;
                cmd.tri[0] = pos;
                cmd.tri[1] = corner(k);
                cmd.tri[2] = corner(k + 1);
                cmd.color = color;
                out.push_back(cmd);
            }
        }

        // a render-time Random roll (Random.insideUnitCircle for the shake)
        float roll01(uint32_t& s) {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            return float(s >> 8) * (1.f / 16777215.f);
        }

        // CreatureLooker (simplified): every so often the closest creature
        // it tracks that is out in the room
        void pick_look(sys::game_state& state, dcon::creature_id c, graphics_state& g, world::room_view const& room) {
            auto& rng = state.world.creature_get_graphics_random(c);
            if (g.look && !creature::here(state, g.look, room.id))
                g.look = dcon::creature_id{};
            if (--g.look_counter > 0)
                return;
            g.look_counter = 30 + rng.range(0, 60);
            auto const& ts = tracker::of(state, c);
            vec2 const m = state.world.body_chunk_get_pos(ch(state, c, 0));
            float best = 3.4e38f;
            g.look = dcon::creature_id{};
            for (uint32_t i = 0; i < ts.count; ++i) {
                auto const o = ts.reps[i].who;
                if (!creature::here(state, o, room.id) || o == c)
                    continue;
                float const d = rw::distance(m, ts.reps[i].last_seen) * (0.75f + 0.5f * rng.value());
                if (d < best) {
                    best = d;
                    g.look = o;
                }
            }
        }
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        auto& g = gs(state, c);
        if (g.cone_count == 0)
            generate_cones(g, prm(state, c), state.world.barnacle_data_get_seed(creature::barnacle_data(state.world, c)));
        vec2 const p0 = state.world.body_chunk_get_pos(ch(state, c, 0));
        // Limb.Reset (they start dangling)
        for (auto& leg : g.legs) {
            leg.pos = p0;
            leg.last_pos = p0;
            leg.vel = vec2{0.f, 0.f};
            leg.absolute_hunt_pos = p0;
            leg.mode = limb_mode::dangle;
            leg.hunt_speed = leg_params.hunt_speed;
            leg.quickness = leg_params.quickness;
        }
        g.last_roll = g.roll;
        g.look = dcon::creature_id{};
        g.look_counter = 0;
    }

    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room) {
        auto& w = state.world;
        auto& g = gs(state, c);
        auto& rng = w.creature_get_graphics_random(c);
        auto const& p = pl(state, c);
        bool const awake = barnacle::consious(state, c);
        float const sub = physics::object_submersion(state, room, barnacle::body(state, c));
        bool const swimming = sub > 0.3f;
        pick_look(state, c, g, room);
        if (g.blink > 0)
            --g.blink;
        if (!awake)
            g.blink = std::max(g.blink, 30);
        if (g.blink == 0 && rng.value() < 0.005f)
            g.blink = std::max(g.blink, rng.range(10, 25));
        g.last_roll = g.roll;
        vec2 const p0 = w.body_chunk_get_pos(ch(state, c, 0));
        vec2 const n = rw::normalized(p0 - w.body_chunk_get_pos(ch(state, c, 1)));
        vec2 const lhs = custom::perpendicular_vector(n);
        // the solid tiles around: it rolls its back to them
        int32_t num = 0;
        vec2 away{0.f, 0.f};
        ivec2 const tp = room_view::get_tile_position(p0);
        for (auto const d : custom::eight_directions) {
            auto const tt = room.get_tile(tp + d);
            if (tt.solid() || tt.t == world::terrain::slope) {
                ++num;
                away -= rw::normalized(rw::to_vec2(d));
            }
        }
        if (sub > 0.f) {
            g.walk_sway += 0.05f;
            g.walk_sway = std::fmod(g.walk_sway, 1.f);
        } else if (p.run_speed < 0.1f) {
            g.walk_sway = custom::lerp_and_tick(g.walk_sway, 0.5f, 0.05f, 0.02f);
        } else {
            g.walk_sway += p.run_speed / 12.f;
            g.walk_sway = std::fmod(g.walk_sway, 1.f);
        }
        float num2 = g.roll;
        if (swimming && sub < 1.f)
            num2 = -n.x;
        else if (swimming)
            num2 = 0.f;
        else if (num > 0)
            num2 = away == vec2{0.f, 0.f} ? 0.f : -mathf::clamp(rw::dot(lhs, rw::normalized(away)) * 2.f, -1.f, 1.f);
        else if (room.get_tile(p0).flags & world::tile_wall_behind)
            num2 = n.x * 0.1f;
        num2 += std::sin(g.walk_sway * mathf::pi * 2.f) * 0.15f
            * custom::lerp_map(rw::magnitude(p0 - w.body_chunk_get_last_pos(ch(state, c, 0))), 0.1f, 5.f, 0.f, 1.f);
        g.roll = custom::lerp_and_tick(g.roll, num2, 0.1f, 0.04f);

        vec2 const v0 = w.body_chunk_get_vel(ch(state, c, 0));
        vec2 const p1 = w.body_chunk_get_pos(ch(state, c, 1));
        for (uint32_t j = 0; j < leg_count; ++j) {
            auto& leg = g.legs[j];
            vec2 const att = leg_attachment(state, c, g, j, 1.f);
            vec2 const target = leg_target(state, c, g, j);
            body_parts::limb_update(leg, leg_params, p0, v0, p1, room);
            body_parts::as_body_part(leg, leg_params.rad,
                [&](player::body_part& bp) { body_parts::connect_to_point(bp, att, leg_length, false, 0.f, v0, 0.1f, 0.f); });
            if (awake && !leg.reached_snap_position && !swimming)
                body_parts::find_grip(leg, room, att, att, leg_length * 1.5f, target, -2, -2, true);
            if (awake && swimming) {
                // paddling
                float const num3 = leg_length / 2.f - 5.f;
                float const f = (g.walk_sway * mathf::pi * 2.f + float(j) / 5.f * mathf::pi * 2.f) * mathf::sign(n.x);
                leg.mode = limb_mode::hunt_absolute_position;
                leg.absolute_hunt_pos = custom::move_towards(target, att, num3) + vec2{std::cos(f), std::sin(f)} * num3;
            } else if (!awake || !custom::dist_less(leg.pos, leg.absolute_hunt_pos, leg_length * 1.5f)) {
                leg.mode = limb_mode::dangle;
                leg.vel += rw::normalized(target - att) * 13.f;
                leg.vel = rw::lerp(leg.vel, target - leg.pos, 0.5f);
            } else {
                leg.vel += rw::normalized(target - att) * 5.f;
            }
        }
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip) {
        auto const& w = state.world;
        auto const& g = gs(state, c);
        auto const& cp = prm(state, c);
        auto const& p = pl(state, c);
        float const t = alpha;
        float const num = mathf::lerp(g.last_roll, g.roll, t);
        vec2 const vector = at_time(state, c, 0, t);
        vec2 vector2 = shell_bottom(state, c, g, t);
        vec2 const normalized = rw::normalized(vector - at_time(state, c, 1, t));
        float const signed_angle = std::atan2(normalized.y, normalized.x) / mathf::deg2rad;
        rotation const rot = make_rotation(signed_angle, num * 90.f + 90.f);
        // the shake (pushing a creature off, the group's unease)
        float num2 = 0.f;
        if (p.shake_time > 0)
            num2 = std::sqrt(float(p.shake_time) / 20.f) * 5.f;
        if (p.group_discomfort > 0.25f)
            num2 = mathf::max(num2, mathf::lerp(0.f, 1.5f, (p.group_discomfort - 0.25f) / 0.75f));
        if (num2 > 0.f && p.has_shell) {
            uint32_t s = (uint32_t(c.index()) * 2654435761u) ^ std::bit_cast<uint32_t>(vector.x) ^ (std::bit_cast<uint32_t>(vector.y) << 7) ^ 0x9E3779B9u;
            float const ang = roll01(s) * 2.f * mathf::pi;
            float const r = std::sqrt(roll01(s));
            vector2 += vec2{std::cos(ang), std::sin(ang)} * r * num2;
        }
        // ApplyPalette (Room.Darkness: the palette's, without the light sources)
        float const darkness = pal.darkness();
        vec3 const black = pal.black();
        vec3 const dark_shell = lerp3(cp.dark_shell_color, black, darkness);
        vec3 const light_shell = lerp3(cp.light_shell_color, black, darkness);

        // the legs (firstLegSprite: A then B for each)
        for (uint32_t j = 0; j < leg_count; ++j) {
            auto const& leg = g.legs[j];
            float num4 = j % 2 == 0 ? 1.f : -1.f;
            if (std::fabs(num) > 0.5f)
                num4 = mathf::sign(num);
            vec2 const att = leg_attachment(state, c, g, j, t);
            vec2 const v5 = rw::lerp(leg.last_pos, leg.pos, t);
            vec2 const v6 = custom::inverse_kinematic(att, v5, leg_length / 2.f, leg_length / 2.f, num4);
            float const sx = -mathf::sign(num4) * cp.leg_thickness;
            sprites::add(out, sprite{"CentipedeLegA", att, custom::aim(att, v6), vec2{sx, rw::distance(att, v6) / 27.f}, vec2{0.5f, 0.1f}, black});
            sprites::add(out, sprite{"CentipedeLegB", v6, custom::aim(v6, v5), vec2{sx, rw::distance(v6, v5) / 25.f}, vec2{0.5f, 0.1f}, black});
        }

        // the body (firstBodySprite: two JaggedCircle discs)
        {
            float const cr = std::fabs(std::cos(num * mathf::pi / 2.f));
            float const r = custom::vec_to_deg(normalized);
            uint32_t const seed = uint32_t(c.index()) * 2246822519u;
            jagged_ellipse(out, vector2 + normalized * 4.f, vec2{inner_rad / 8.f * mathf::lerp(0.5f, 0.7f, cr), inner_rad / 8.f * 0.9f}, r, seed, black);
            jagged_ellipse(out, vector2 - normalized * 3.f, vec2{inner_rad / 8.f * mathf::lerp(0.4f, 0.6f, cr), inner_rad / 8.f * 0.6f}, r, seed ^ 0x5bd1e995u, black);
        }

        // the eyes on their stalks (firstEyeSprite: the two stalks, then the
        // two pixels), bare only
        if (!p.has_shell) {
            vec2 v3[2], v4[2];
            vec2 look{0.f, 0.f};
            if (g.look && creature::live(state, g.look)) {
                vec2 const lp = creature::main_pos(state, g.look);
                look = rw::clamp_magnitude((lp - vector2) / 70.f, 1.f);
            }
            for (uint32_t i = 0; i < 2; ++i) {
                float const side = float(i) * 2.f - 1.f;
                v3[i] = xy(rot * vec3{inner_rad * 0.8f, inner_rad * 0.1f, side * 1.5f}) + vector2;
                v4[i] = xy(rot * vec3{inner_rad * 1.3f, inner_rad * 0.3f, side * 2.5f}) + vector2 + look * 4.f;
            }
            for (uint32_t i = 0; i < 2; ++i)
                sprites::add(out, sprite{"LizardScaleA5", v4[i], custom::aim(v4[i], v3[i]), vec2{1.f, rw::distance(v4[i], v3[i]) / 29.f}, vec2{0.5f, 0.f}, black});
            vec3 const pupil = lerp3(black, vec3{1.f, 1.f, 1.f}, 0.25f);
            float const ps = g.blink > 0 ? 1.f : 2.f;
            for (uint32_t i = 0; i < 2; ++i)
                sprites::add(out, sprite{"pixel", v4[i], custom::vec_to_deg(normalized), vec2{ps, ps}, vec2{0.5f, 0.5f}, pupil});
        }

        // the shell's cones, back to front (tempData sorted by from.z)
        if (p.has_shell) {
            uint32_t const n = g.cone_count;
            uint8_t order[barnacle::max_cones];
            vec3 from[barnacle::max_cones], to[barnacle::max_cones];
            for (uint32_t l = 0; l < n; ++l) {
                order[l] = uint8_t(l);
                from[l] = rot * g.cones[l].base_pos;
                to[l] = rot * g.cones[l].tip_pos;
            }
            std::stable_sort(order, order + n, [&](uint8_t a, uint8_t b) { return from[a].z < from[b].z; });
            for (uint32_t q = 0; q < n; ++q) {
                uint32_t const l = order[q];
                auto const& k = g.cones[l];
                vec2 const v7 = vector2 + xy(from[l]);
                vec2 const v8 = vector2 + xy(to[l]);
                vec2 const a3 = custom::perpendicular_vector(rw::normalized(v8 - v7));
                vec3 const color = lerp3(dark_shell, light_shell, k.lightness);
                float const rot2 = custom::aim(v7, v8);
                float const z = normalized3(from[l]).z;
                bool const back = z < 0.f;
                // the TriangleMesh (ConeContour: t^1.5 from the base's to the tip's radius)
                {
                    vec2 const v[4] = {v7 + a3 * k.base_rad, v7 - a3 * k.base_rad, v8 + a3 * k.tip_rad, v8 - a3 * k.tip_rad};
                    sprites::long_mesh(out, strip, v, 4, color);
                }
                sprite const base_disc{"Circle20", v7, rot2, vec2{k.base_rad / 10.f, k.base_rad / 10.f * std::fabs(z)}, vec2{0.5f, 0.5f}, back ? black : color};
                sprite const tip_disc{"Circle20", v8, rot2, vec2{k.tip_rad / 10.f, k.tip_rad / 10.f * std::fabs(z)}, vec2{0.5f, 0.5f}, color};
                // facing away, the tip goes first (sprite num6 + 1)
                sprites::add(out, back ? tip_disc : base_disc);
                sprites::add(out, back ? base_disc : tip_disc);
                float const ez = mathf::max(0.f, z);
                sprites::add(out, sprite{"Circle20", v8, rot2, vec2{k.eye_rad / 10.f, k.eye_rad / 10.f * ez}, vec2{0.5f, 0.5f}, lerp3(color, black, 0.5f)});
                sprites::add(out, sprite{"Circle20", rw::lerp(v8, v7, 0.1f), rot2, vec2{k.eye_rad / 10.f, k.eye_rad / 10.f * ez}, vec2{0.5f, 0.5f}, black});
            }
        }
        (void)w;
    }
}
