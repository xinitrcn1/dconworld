#include <cmath>
#include <optional>
#include <string>

#include "cicada_graphics.hpp"
#include "rooms.hpp"
#include "body_parts.hpp"
#include "cicada.hpp"
#include "player.hpp"
#include "tracker.hpp"
#include "player_graphics.hpp"

namespace cicada_graphics {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using world::room_view;
    using player::limb_mode;
    using sprites::sprite;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        // Limb(rad 0, surface friction 0.7, air friction 0.9, hunt speed 6, quickness 0.5)
        constexpr body_parts::limb_params tentacle{0.f, 0.7f, 0.9f, 6.f, 0.5f};

        vec2 chunk_pos(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_pos(cicada::chunk(state, c, i));
        }
        vec2 chunk_last_pos(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_last_pos(cicada::chunk(state, c, i));
        }
        vec2 chunk_vel(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_vel(cicada::chunk(state, c, i));
        }

        dcon::player_id player_of(sys::game_state const& state, uint8_t v) {
            if (v == 0)
                return dcon::player_id{};
            dcon::player_id const p{dcon::player_id::value_base_t(v - 1)};
            return state.world.player_is_valid(p) && rooms::reachable(state, p) ? p : dcon::player_id{};
        }

        // Color.Lerp
        vec3 mix(vec3 a, vec3 b, float t) {
            return a + (b - a) * mathf::clamp01(t);
        }

        // CicadaGraphics.ResetWing: snapped out to the side of the body
        void reset_wing(sys::game_state& state, dcon::creature_id c, int side, int wing) {
            auto& g = state.world.cicada_data_get_graphics(creature::cicada_data(state.world, c));
            vec2 const p0 = chunk_pos(state, c, 0);
            vec2 const v = custom::dir_vec(chunk_pos(state, c, 1), p0);
            vec2 const a = custom::perpendicular_vector(v);
            auto& wg = g.wings[side][wing];
            wg.vel = vec2{0.f, 0.f};
            wg.pos = p0 - v * 5.f;
            wg.pos += (wing == 0 ? -3.f : 10.f) * v;
            wg.pos += 17.f * a * (side == 0 ? -1.f : 1.f) * std::fabs(g.z_rotation.y);
            wg.pos += 17.f * a * g.z_rotation.x;
            body_parts::connect_to_point(wg, p0 - v * 5.f, wing == 0 ? 23.f : 17.f, true, 0.f, chunk_vel(state, c, 0), 0.f, 0.f);
        }
    }

    void colors(sys::game_state const& state, dcon::creature_id c, palette const& pal, vec3& body, vec3& shield, vec3& eyes) {
        auto const& w = state.world;
        float const hue = w.cicada_data_get_ivars(creature::cicada_data(w, c)).hue;
        bool const gender = w.creature_get_payload(c).cicada.gender != 0;
        // HSLColor.Lerp towards white (CicadaA) or black (CicadaB), then the palette
        if (gender) {
            body = mix(custom::hsl2rgb(hue, 0.2f, 0.9f), pal.fog, 0.1f);
            shield = mix(body, custom::hsl2rgb(hue, 0.5f, 0.4f), 0.8f);
            eyes = mix(body, pal.black, 0.8f);
        } else {
            body = mix(custom::hsl2rgb(hue, 0.2f, 0.1f), pal.black, 0.85f);
            shield = mix(body, custom::hsl2rgb(hue, 0.5f, 0.5f), 0.4f);
            eyes = custom::hsl2rgb(hue, 1.f, 0.5f);
        }
    }

    void reset(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        auto& g = w.cicada_data_get_graphics(creature::cicada_data(w, c));
        g = cicada::graphics_state{};
        // vanilla seeds these from the creature's ID
        w.creature_get_graphics_random(c) = rw::rng{w.creature_get_random(c).state ^ 0x5bd1e995u};
        auto& rng = w.creature_get_graphics_random(c);
        auto const& iv = w.cicada_data_get_ivars(creature::cicada_data(w, c));
        g.wing_offset = rng.value();
        vec2 const p0 = chunk_pos(state, c, 0);
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 2; ++j) {
                g.wings[i][j] = player::body_part{};
                g.wings[i][j].rad = 0.f;
                auto& t = g.tentacles[i][j];
                t = player::hand_state{};
                t.mode = limb_mode::dangle;
                t.hunt_speed = tentacle.hunt_speed;
                t.quickness = tentacle.quickness;
                g.wing_deployment[i][j] = iv.default_wing_deployment;
            }
        g.lazy_wing = rng.range(-2, 4);
        // Reset: every body part at the head
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 2; ++j) {
                g.wings[i][j].pos = g.wings[i][j].last_pos = p0;
                g.tentacles[i][j].pos = g.tentacles[i][j].last_pos = p0;
                g.tentacles[i][j].absolute_hunt_pos = p0;
            }
        g.z_rotation = g.last_z_rotation = custom::dir_vec(chunk_pos(state, c, 1), p0);
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        auto const keep_random = w.creature_get_graphics_random(c);
        auto const keep = w.cicada_data_get_graphics(creature::cicada_data(w, c));
        reset(state, c);
        auto& g = w.cicada_data_get_graphics(creature::cicada_data(w, c));
        w.creature_set_graphics_random(c, keep_random);
        g.wing_offset = keep.wing_offset;
        g.lazy_wing = keep.lazy_wing;
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto& g = w.cicada_data_get_graphics(creature::cicada_data(w, c));
        auto& rng = w.creature_get_graphics_random(c);
        auto const& iv = w.cicada_data_get_ivars(creature::cicada_data(w, c));
        vec2 const p0 = chunk_pos(state, c, 0), p1 = chunk_pos(state, c, 1);
        vec2 const v0 = chunk_vel(state, c, 0);
        bool const awake = cicada::consious(state, c);
        bool const flying = w.creature_get_payload(c).cicada.flying != 0;
        int32_t const charge_counter = w.creature_get_payload(c).cicada.charge_counter;

        // the wing sounds: the hum (a ChunkSoundEmitter kept up while the
        // wings are out) and how it stops
        float spread = 0.f;
        for (auto const& side : g.wing_deployment)
            for (float const d : side)
                if (d == 1.f)
                    spread += 0.25f;
        spread = mathf::pow(spread, 1.4f);
        uint8_t loop = 0;
        if (awake && spread > 0.f)
            loop = charge_counter > 0 ? (cicada::charging(state, c) ? 3 : 2) : 1;
        if (loop != g.current_loop) {
            if (loop == 0 && spread > 0.f)
                state.play_sound(awake ? sound::sound_id::cicada_wings_stop : sound::sound_id::cicada_wings_violent_stop, room.id, p0, spread, iv.wing_sound_pitch);
            g.current_loop = loop;
        }
        if (loop != 0) {
            using sound::sound_id;
            sound_id const id = loop == 1 ? sound_id::cicada_wings_loop
                : loop == 3 ? sound_id::cicada_wings_bump_attack_charge_loop : sound_id::cicada_wings_bump_attack_prepare_loop;
            float pitch = iv.wing_sound_pitch;
            if (loop == 1)
                pitch *= 0.85f + 0.3f * mathf::pow(mathf::inverse_lerp(-1.f, 1.f, rw::dot(rw::normalized(p0 - p1), rw::normalized(g.z_rotation))), 2.f);
            state.play_loop(sound::loop_key(sound::loop_owner::creature, uint32_t(c.index())), id, room.id, p0, spread, pitch);
        }

        // CreatureLooker: what it is busy with, else what it hunts and sees
        auto look = w.cicada_data_get_focus(creature::cicada_data(w, c));
        auto const& ts = tracker::of(state, c);
        if (!look)
            if (auto const* r = tracker::prey(ts); r && r->visual)
                look = r->who;
        g.look_creature = look;

        g.charging_visuals = charge_counter > 0 ? mathf::min(g.charging_visuals + 0.1f, 1.f) : mathf::max(g.charging_visuals - 0.05f, 0.f);
        g.last_z_rotation = g.z_rotation;
        g.z_rotation = rw::lerp(g.z_rotation, custom::dir_vec(p1, p0), 0.15f);
        if (awake) {
            --g.blink_counter;
            if (g.blink_counter < -15 || (g.blink_counter < -2 && rng.value() < 0.33333334f))
                g.blink_counter = rng.range(10, 300);
            vec2 const last0 = chunk_last_pos(state, c, 0);
            if (!flying && !custom::dist_less(p0, last0, 0.4f)) {
                // a pull of a tentacle every so often while climbing
                g.climb_counter += rw::distance(p0, last0);
                if (g.climb_counter > 30.f) {
                    w.body_chunk_get_vel(cicada::chunk(state, c, 0)) += custom::deg_to_vec(rng.value() * 360.f);
                    state.play_sound(sound::sound_id::cicada_tentacle_climb, room.id, p0);
                    g.climb_counter = 0.f;
                }
            }
            if (flying) {
                g.wing_deployment_get_to = 1.f;
                if (charge_counter > 0) {
                    g.blink_counter = -5;
                    g.z_rotation.y -= 0.5f;
                } else if (std::fabs(p1.x - p0.x) > 5.f) {
                    g.z_rotation += custom::deg_to_vec(p1.x < p0.x ? 112.5f : -112.5f) * 0.3f;
                }
            } else {
                ivec2 const sit = w.cicada_data_get_sit_direction(creature::cicada_data(w, c));
                if (sit.x != 0)
                    g.z_rotation += vec2{0.4f * float(-sit.x) * (p1.y > p0.y ? 1.f : -1.f), 0.f};
                else
                    g.z_rotation += vec2{0.4f * float(sit.y) * (p1.x > p0.x ? 1.f : -1.f), 0.f};
                if (w.creature_get_payload(c).cicada.wait_to_fly_counter > 0)
                    g.wing_deployment_get_to = 0.9f;
                else if (g.wing_deployment_get_to == 1.f)
                    g.wing_deployment_get_to = 0.9f;
                else if (rng.value() < 0.071428575f)
                    g.wing_deployment_get_to = mathf::max(0.f, g.wing_deployment_get_to - rng.value() / 6.f);
            }
        } else {
            g.blink_counter = -5;
        }
        if (rng.value() < 0.025f)
            g.lazy_wing = rng.range(-2, 4);
        g.z_rotation = rw::normalized(g.z_rotation);
        vec2 const dir = custom::dir_vec(p1, p0);
        vec2 const perp = custom::perpendicular_vector(dir);
        auto const* looked = g.look_creature ? tracker::find(ts, g.look_creature) : nullptr;
        if (awake && looked && g.blink_counter > 0) {
            if (looked->visual && creature::here(state, looked->who, room.id))
                g.look_dir = custom::dir_vec(p0, creature::main_pos(state, looked->who));
            else
                g.look_dir = custom::dir_vec(p0, room_view::middle_of_tile(looked->last_seen));
            vec2 const v2 = body_parts::rotate_around_origo(g.look_dir, custom::aim(vec2{0.f, 0.f}, vec2{-dir.x, dir.y}));
            g.look_rotation = custom::aim(-v2, v2);
        } else {
            g.look_dir *= 0.9f;
            g.look_rotation *= 0.8f;
        }

        // the wings: deployed (fluttering, drawn from the wing timer) or
        // folded back, trailing the body
        float const d = awake ? 1.f : 0.5f;
        float const power = w.creature_get_payload(c).cicada.flying_power;
        float const wing_length = iv.wing_length;
        for (int k = 0; k < 2; ++k)
            for (int l = 0; l < 2; ++l) {
                float& dep = g.wing_deployment[k][l];
                int32_t const n = k + l + l;
                if (awake && iv.busted_wing != n) {
                    if (rng.value() < 0.033333335f)
                        g.wing_deployment_speed[k][l] = rng.value() * rng.value() * 0.3f;
                    float const speed = g.wing_deployment_speed[k][l];
                    if (g.wing_deployment_get_to == 1.f && g.lazy_wing != n) {
                        dep = 1.f;
                    } else if (dep < g.wing_deployment_get_to) {
                        dep = mathf::min(dep + speed, g.wing_deployment_get_to);
                        if (w.creature_get_payload(c).cicada.wait_to_fly_counter > 0 && g.lazy_wing != n)
                            dep = mathf::min(dep + speed * 2.4f, g.wing_deployment_get_to);
                    } else if (dep > g.wing_deployment_get_to) {
                        if (dep == 1.f)
                            reset_wing(state, c, k, l);
                        dep = mathf::max(dep - speed, g.wing_deployment_get_to);
                    }
                } else if (dep == 1.f) {
                    dep = 0.9f;
                    reset_wing(state, c, k, l);
                }
                if (dep < 1.f) {
                    auto& wg = g.wings[k][l];
                    wg.last_pos = wg.pos;
                    wg.pos += wg.vel;
                    wg.vel *= 0.8f;
                    float const t = mathf::inverse_lerp(0.5f, 1.f, dep) * power;
                    float const side = k == 0 ? -1.f : 1.f;
                    wg.vel -= (l == 0 ? 0.6f : 0.3f) * dir * d * mathf::lerp(1.f, l == 0 ? 0.f : -5.5f, t);
                    wg.vel += 0.2f * perp * side * std::fabs(g.z_rotation.y) * d * mathf::lerp(1.f, 6.f, t);
                    wg.vel += 0.2f * perp * g.z_rotation.x * d * mathf::lerp(1.f, 6.f, t);
                    if (!awake)
                        wg.vel.y -= 0.3f;
                    if (dep < 0.5f) {
                        float const fold = mathf::inverse_lerp(0.5f, 0.f, dep);
                        vec2 v3 = p0 - dir * (l == 0 ? 20.f : 15.f);
                        v3 += -2.f * perp * side * std::fabs(g.z_rotation.y);
                        v3 += 5.f * perp * g.z_rotation.x;
                        wg.vel *= 1.f - fold;
                        wg.pos = rw::lerp(wg.pos, v3, fold);
                    }
                    body_parts::connect_to_point(wg, p0 - dir * 5.f, (l == 0 ? 23.f : 17.f) * wing_length, true, 0.f, v0, 0.5f, 0.f);
                    body_parts::push_out_of_terrain(wg, room, 0.f);
                }
            }

        // the tentacles
        auto const holder = player_of(state, w.creature_get_payload(c).cicada.held_by);
        auto const sticky = w.creature_get_payload(c).cicada.sticky;
        std::optional<dcon::body_chunk_id> cling;
        if (sticky && creature::live(state, sticky) && w.creature_get_payload(c).cicada.sticky_chunk < creature::chunk_count_of(state, sticky))
            cling = creature::chunk_of(state, sticky, w.creature_get_payload(c).cicada.sticky_chunk);
        for (int m = 0; m < 2; ++m)
            for (int n = 0; n < 2; ++n) {
                auto& t = g.tentacles[m][n];
                float const reach = (n == 0 ? 24.f : 19.f) * iv.tentacle_length;
                t.vel.y -= 0.6f;
                t.vel += dir * 0.55f;
                t.vel += (n == 0 ? 0.2f : 0.6f) * perp * (m == 0 ? -1.f : 1.f) * std::fabs(g.z_rotation.y);
                t.vel -= (n == 0 ? -0.1f : 0.6f) * perp * g.z_rotation.x;
                body_parts::limb_update(t, tentacle, p0, v0, p1, room);
                if (n == 1 && holder) {
                    // the back pair wraps around the slugcat's hands
                    t.pos = player_graphics::hand_position(state, holder, uint32_t(m));
                    continue;
                }
                bool connect = true;
                if (cling && (flying || n == 0)) {
                    vec2 const cp = w.body_chunk_get_pos(*cling);
                    float const ang = (n == 1 ? 45.f : 135.f) * (m == 0 ? -1.f : 1.f);
                    t.mode = limb_mode::hunt_absolute_position;
                    t.absolute_hunt_pos = cp + custom::deg_to_vec(custom::aim(p0, cp) + ang) * w.body_chunk_get_rad(*cling);
                    connect = !t.reached_snap_position && !custom::dist_less(t.pos, p0, 40.f);
                } else if (flying || !awake) {
                    t.mode = limb_mode::dangle;
                } else {
                    // reaching out for grips around the head while sitting or climbing
                    float ang = ((n == 1) == (g.climb_counter > 15.f)) ? 20.f : 160.f;
                    if (cling)
                        ang = ((m == 1) == (g.climb_counter > 15.f)) ? 20.f : 160.f;
                    if (m == 0)
                        ang *= -1.f;
                    ang += custom::aim(-dir, dir);
                    vec2 const v4 = p0 - dir * 10.f + custom::deg_to_vec(ang) * reach;
                    if (t.mode == limb_mode::dangle || !custom::dist_less(t.pos, v4, 25.f))
                        body_parts::find_grip(t, room, v4, v4, reach * 1.2f, v4, -2, -2, false);
                }
                if (connect)
                    body_parts::as_body_part(t, tentacle.rad, [&](player::body_part& bp) {
                        body_parts::connect_to_point(bp, p0 + dir * 10.f, reach, false, 0.f, v0, 0.f, 0.f);
                        body_parts::push_out_of_terrain(bp, room, tentacle.surface_friction);
                    });
            }

        g.wing_offset += 1.f / float(rng.range(50, 60));
        g.wing_time_add += 1.f;
        if (g.wing_time_add >= 3.f)
            g.wing_time_add = 0.f;
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, palette const& pal, std::vector<sprites::draw_cmd>& out) {
        auto const& w = state.world;
        auto const& g = w.cicada_data_get_graphics(creature::cicada_data(w, c));
        auto const& iv = w.cicada_data_get_ivars(creature::cicada_data(w, c));
        bool const gender = w.creature_get_payload(c).cicada.gender != 0;

        // ApplyPalette
        vec3 body, shield, eye;
        colors(state, c, pal, body, shield, eye);
        vec3 const highlight = gender ? mix(body, vec3{1.f, 1.f, 1.f}, 0.7f) : mix(body, custom::hsl2rgb(iv.hue, 1.f, 0.5f), 0.07f);
        vec3 const eyes_b = gender ? custom::hsl2rgb(iv.hue, 1.f, 0.5f) : pal.black;

        // Vector3.Slerp
        vec2 const zr = rw::slerp(g.last_z_rotation, g.z_rotation, alpha);
        vec2 const h0 = rw::lerp(chunk_last_pos(state, c, 0), chunk_pos(state, c, 0), alpha);
        vec2 const h1 = rw::lerp(chunk_last_pos(state, c, 1), chunk_pos(state, c, 1), alpha);
        vec2 const mid = rw::lerp(h0, h1, 0.5f);
        vec2 const n = rw::normalized(h0 - h1);
        vec2 const a = custom::perpendicular_vector(-n);
        float const body_deg = custom::aim(-n, n);
        float const turn = custom::aim(vec2{0.f, 0.f}, zr);
        // one of the nine angles the body is drawn at, and the rotation that
        // makes up for the rest of the turn
        int32_t const angle = custom::int_clamp(8 - int32_t(std::fabs(turn / 180.f) * 9.f), 0, 8);
        float const angle_rot = float(8 - angle) * mathf::sign(turn) * 22.5f;
        float const flip = turn > 0.f ? -1.f : 1.f;

        // the elements by name ("Cicada" + angle + part), kept alive for the frame
        static char const* const parts[5] = {"body", "head", "shield", "eyes1", "eyes2"};
        static std::string names[9][5];
        if (names[0][0].empty())
            for (int i = 0; i < 9; ++i)
                for (int p = 0; p < 5; ++p)
                    names[i][p] = "Cicada" + std::to_string(i) + parts[p];

        // BodySprite (0), HeadSprite (6), ShieldSprite (7), EyesASprite (8), EyesBSprite (9)
        auto const head_part = [&](int part, float offset, float rot_k, vec3 col, float scale_y) {
            return sprite{names[angle][part], mid + g.look_dir * offset, body_deg + g.look_rotation * rot_k - angle_rot, vec2{flip, scale_y}, vec2{0.5f, 0.5f}, col};
        };
        sprite const body_sprite = head_part(0, 0.f, 0.f, body, iv.fatness);
        sprite const head_sprite = head_part(1, 1.f, 0.1f, body, 1.f);
        sprite const shield_sprite = head_part(2, 0.f, 0.05f, shield, 1.f);
        sprite const eyes_a = head_part(3, 2.f, 0.1f, g.blink_counter > 0 ? eye : shield, 1.f);
        sprite const eyes_b_sprite = head_part(4, 3.f, 0.1f, eyes_b, 1.f);

        // HighlightSprite: a shine on the back
        float const fat = std::fabs(iv.fatness - 1.f) * 10.f;
        vec2 const hp = h1 + vec2{-2.f, 3.f};
        sprite const highlight_sprite{"Circle20", hp, body_deg + 12.f, vec2{mathf::lerp(5.f, 3.f, fat) / 20.f, mathf::lerp(12.f, 8.f, fat) / 20.f}, vec2{0.5f, 0.5f}, highlight};

        // WingSprites: the CicadaWing shader isn't available, the wings are
        // drawn as the sprite tinted and faded by its alpha
        sprite wings[2][2];
        vec4 wing_cols[2][2];
        for (int j = 0; j < 2; ++j) {
            float off = j == 0 ? 5.f : 11.f;
            float const base = j == 0 ? -20.f : 24.f;
            off += 3.f * std::fabs(zr.x);
            for (int k = 0; k < 2; ++k) {
                float const dep = g.wing_deployment[k][j];
                float const side = k == 0 ? 1.f : -1.f;
                float const dd = (j == 0 ? 11.f : 9.f) * (0.2f + 0.8f * std::fabs(zr.y)) * mathf::lerp(1.f, 0.85f, mathf::inverse_lerp(0.5f, 0.f, dep));
                vec2 v3 = mid + n * off + a * dd * side + a * zr.x * mathf::lerp(-3.f, -5.f, mathf::inverse_lerp(0.5f, 0.f, dep));
                v3 += g.look_dir * (j == 0 ? 1.f : 3.f);
                vec2 const wp = rw::lerp(g.wings[k][j].last_pos, g.wings[k][j].pos, alpha);
                float shown = zr.x;
                if (dep < 1.f)
                    shown = mathf::max(mathf::inverse_lerp(30.f, 18.f, rw::distance(chunk_pos(state, c, 1), g.wings[k][j].pos)), mathf::inverse_lerp(1.f, 0.5f, dep));
                shown = mathf::lerp(shown, 1.f, g.charging_visuals);
                vec3 const col = mix(vec3{0.f, 0.f, 0.f}, shield, std::fabs(shown) + 0.2f);
                float const wa = mathf::pow(std::fabs(shown), 3.f);
                float rot, sx;
                if (dep == 1.f) {
                    // the flap, from the wing timer
                    float const timer = mathf::inverse_lerp(0.f, 3.f, g.wing_time_add + alpha);
                    float flap, lo, hi;
                    if (j == 0) {
                        flap = mathf::pow(custom::decimal(g.wing_offset + timer), 0.75f);
                        lo = -65.f;
                        hi = 40.f;
                    } else {
                        flap = mathf::pow(custom::decimal(g.wing_offset + timer + 0.8f), 1.3f);
                        lo = -45.f;
                        hi = 75.f;
                    }
                    flap = mathf::pow(0.5f + 0.5f * std::sin(flap * mathf::pi * 2.f) * mathf::lerp(1.f, 0.3f, g.charging_visuals), 0.7f);
                    flap = mathf::lerp(lo, hi, flap);
                    if (g.charging_visuals > 0.f)
                        flap += cicada::charging(state, c) ? -60.f * g.charging_visuals : 30.f * g.charging_visuals;
                    rot = body_deg - 180.f + (base + flap) * (k == 0 ? -1.f : 1.f);
                    sx = mathf::max(0.f, mathf::lerp(std::fabs(zr.y), 1.f, std::fabs(0.5f - flap) * 1.4f)) * (k == 0 ? -1.f : 1.f) * iv.wing_length;
                } else {
                    sx = (k == 0 ? -1.f : 1.f) * iv.wing_length;
                    rot = custom::aim(v3, wp) - 90.f * (k == 0 ? -1.f : 1.f);
                }
                wings[k][j] = sprite{j == 0 ? "CicadaWingA" : "CicadaWingB", v3, rot, vec2{sx, iv.wing_thickness}, vec2{0.f, 0.5f}, col};
                wing_cols[k][j] = vec4{col.x, col.y, col.z, wa};
            }
        }

        // TentacleSprites: three segment long meshes with a pointed tip, from
        // under the head out to the tentacle's end
        vec2 meshes[2][2][11];
        {
            vec2 root = h0 + n * (8.f + 2.f * std::fabs(zr.x) * (1.f - rw::magnitude(g.look_dir)));
            root += a * zr.x * (3.f - 2.f * rw::magnitude(g.look_dir));
            root += g.look_dir * 6.f;
            for (int l = 0; l < 2; ++l)
                for (int m = 0; m < 2; ++m) {
                    vec2 const tip = rw::lerp(g.tentacles[m][l].last_pos, g.tentacles[m][l].pos, alpha);
                    vec2 prev = root;
                    if (l == 0)
                        prev += n * 3.f * std::fabs(zr.x);
                    float prev_w = 2.f * iv.tentacle_thickness;
                    float bend = 1.f;
                    vec2* v = meshes[m][l];
                    for (int k = 0; k < 3; ++k) {
                        vec2 p = tip;
                        if (k < 2) {
                            p = rw::lerp(root, tip, float(k + 1) / 3.f);
                            p += n * bend * 15.f * mathf::inverse_lerp(1.f, -1.f, rw::dot(n, rw::normalized(p - prev)));
                            if (l == 1 && k == 0)
                                p += a * zr.x * bend * 5.f;
                            p += a * std::fabs(zr.y) * (m == 0 ? 1.f : -1.f) * bend * (l == 1 ? 3.f : 1.5f);
                        }
                        bend *= 0.5f;
                        vec2 const nd = rw::normalized(p - prev);
                        vec2 const pa = custom::perpendicular_vector(nd);
                        float const len = rw::distance(p, prev) / (k == 0 ? 1.f : 5.f);
                        float const wd = (k < 1 ? 1.2f : 0.8f) * iv.tentacle_thickness;
                        v[k * 4] = prev - pa * (wd + prev_w) * 0.5f + nd * len;
                        v[k * 4 + 1] = prev + pa * (wd + prev_w) * 0.5f + nd * len;
                        if (k < 2) {
                            v[k * 4 + 2] = p - pa * wd - nd * len;
                            v[k * 4 + 3] = p + pa * wd - nd * len;
                        } else {
                            v[k * 4 + 2] = p + nd * 2.1f;
                        }
                        prev_w = wd;
                        prev = p;
                    }
                }
        }

        // in vanilla sprite order: BodySprite, HighlightSprite, TentacleSprite
        // (side, tentacle) at 2 + side + 2 tentacle, HeadSprite, ShieldSprite,
        // EyesASprite, EyesBSprite (hidden mid blink), WingSprite (side, wing)
        // at 10 + side + 2 wing
        sprites::add(out, body_sprite);
        sprites::add(out, highlight_sprite);
        for (int l = 0; l < 2; ++l)
            for (int m = 0; m < 2; ++m)
                sprites::long_mesh(out, meshes[m][l], 11, body);
        sprites::add(out, head_sprite);
        sprites::add(out, shield_sprite);
        sprites::add(out, eyes_a);
        if (g.blink_counter >= 0)
            sprites::add(out, eyes_b_sprite);
        for (int j = 0; j < 2; ++j)
            for (int k = 0; k < 2; ++k) {
                vec4 const corners[4] = {wing_cols[k][j], wing_cols[k][j], wing_cols[k][j], wing_cols[k][j]};
                sprites::add(out, wings[k][j], corners);
            }
    }
}
