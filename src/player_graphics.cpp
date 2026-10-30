#include <array>
#include <cmath>
#include <string>

#include "player_graphics.hpp"
#include "body_parts.hpp"
#include "cicada.hpp"
#include "items.hpp"
#include "physics.hpp"
#include "creature.hpp"
#include "player.hpp"
#include "rot.hpp"

namespace player_graphics {
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    using world::terrain;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;
    using body_parts::connect_to_point;
    using body_parts::find_grip;
    using body_parts::generic_update;
    using body_parts::push_out_of_terrain;
    using body_parts::rotate_around_origo;
    using custom::aim;
    using rw::slerp;

    namespace {
        // GenericBodyPart / TailSegment constants (PlayerGraphics constructor)
        constexpr float head_rad = 4.f, head_surface_friction = 0.8f, head_air_friction = 0.99f;
        constexpr float legs_rad = 1.f, legs_surface_friction = 0.8f, legs_air_friction = 0.99f;
        constexpr float tail_rad[4] = {6.f, 4.f, 2.5f, 1.f};
        constexpr float tail_connection_rad[4] = {4.f, 7.f, 7.f, 7.f};
        // PlayerGraphics.InitializeLongerWatcherTail
        constexpr float weaver_tail_connection_rad[4] = {6.f, 10.5f, 10.5f, 10.5f};
        // SlugcatHand: Limb(rad 3, surface friction 0.8, air friction 1, hunt speed 7, quickness 0.5)
        constexpr body_parts::limb_params hand{3.f, 0.8f, 1.f, 7.f, 0.5f};
        constexpr float tail_affect_previous[4] = {1.f, 0.5f, 0.5f, 0.5f};
        constexpr float tail_surface_friction = 0.85f, tail_air_friction = 1.f;

        // element names with stable storage (sprites refer to them by view)
        template<size_t N>
        std::array<std::string, N> make_names(char const* prefix) {
            std::array<std::string, N> r;
            for (size_t i = 0; i < N; ++i)
                r[i] = prefix + std::to_string(i);
            return r;
        }
        auto const head_names = make_names<18>("HeadA");
        auto const face_a_names = make_names<9>("FaceA");
        auto const face_b_names = make_names<9>("FaceB");
        auto const legs_names = make_names<7>("LegsA");
        auto const legs_crawling_names = make_names<6>("LegsACrawling");
        auto const legs_climbing_names = make_names<7>("LegsAClimbing");
        auto const legs_on_pole_names = make_names<7>("LegsAOnPole");
        auto const arm_names = make_names<13>("PlayerArm");

        // TailSegment.Update; segment 0 hangs from `point`
        void tail_update(player::graphics_state& g, int i, vec2 point, room_view const& room) {
            auto& t = g.tail[i];
            t.last_pos = t.pos;
            t.pos += t.vel;
            t.vel *= tail_air_friction;
            t.stretched = 1.f;
            float const cr = (g.weaver ? weaver_tail_connection_rad : tail_connection_rad)[i];
            if (i > 0) {
                auto& prev = g.tail[i - 1];
                if (!custom::dist_less(t.pos, prev.pos, cr)) {
                    float const d = rw::distance(t.pos, prev.pos);
                    vec2 const a = custom::dir_vec(t.pos, prev.pos);
                    float const ap = tail_affect_previous[i];
                    t.pos -= a * ((cr - d) * (1.f - ap));
                    t.vel -= a * ((cr - d) * (1.f - ap));
                    prev.pos += a * ((cr - d) * ap);
                    prev.vel += a * ((cr - d) * ap);
                    t.stretched = mathf::clamp((cr / (d * 0.5f) + 2.f) / 3.f, 0.2f, 1.f);
                }
            } else if (!custom::dist_less(t.pos, point, cr)) {
                float const d = rw::distance(t.pos, point);
                vec2 const a = custom::dir_vec(t.pos, point);
                t.pos -= a * (cr - d);
                t.vel -= a * (cr - d);
                t.stretched = mathf::clamp((cr / (d * 0.5f) + 2.f) / 3.f, 0.2f, 1.f);
            }
            push_out_of_terrain(t, room, tail_surface_friction);
        }

        bool consious(sys::game_state const& state, dcon::player_id p) {
            return player::stun_of(state, p) <= 0 && !player::dead(state, p);
        }

        // AxolotlGills constructor constants, per scale (scalesPositions[i * 2 + j])
        struct gill_shape {
            float length, width, backwards, y;
        };
        constexpr float gill_rigor = 0.5873646f;
        gill_shape gill(int s) {
            constexpr float rows[3][4] = { // y, x, length factor, backwards factor
                {0.03570603f, 0.659981f, 0.9722961f, 0.3644831f},
                {0.02899241f, 0.76459f, 0.6056554f, 0.9129724f},
                {0.02639332f, 0.7482835f, 0.7223744f, 0.4567381f},
            };
            auto const& r = rows[s / 2];
            return gill_shape{mathf::lerp(2.5f, 15.f, 1.310689f * r[2]), mathf::lerp(0.65f, 1.2f, 0.1542603f * 1.310689f),
                0.1759363f * r[3], r[0]};
        }

        // AxolotlGills.Update: six scales on the sides of the head
        void gills_update(player::graphics_state& g, vec2 c0, vec2 c1, room_view const& room) {
            for (int i = 0; i < 6; ++i) {
                auto& sc = g.gills[i];
                auto const shape = gill(i);
                vec2 pos = c0;
                pos.x += i >= 3 ? 5.f : -5.f;
                float const step = i % 3;
                vec2 const a = custom::deg_to_vec(step * 30.f - 45.f + 90.f);
                vec2 const fan = custom::deg_to_vec(step * 30.f - 45.f);
                // vanilla lerps by the look direction's angle in degrees,
                // so any look direction means "along the body"
                float const look_deg = std::atan2(g.look_direction.x, g.look_direction.y) * 57.29578f;
                vec2 dir = rw::lerp(fan, custom::dir_vec(c1, c0), std::fabs(look_deg));
                if (shape.y < 0.2f)
                    dir -= a * mathf::pow(mathf::inverse_lerp(0.2f, 0.f, shape.y), 2.f) * 2.f;
                dir = rw::normalized(rw::lerp(dir, fan, shape.backwards));
                vec2 const goal = pos + dir * shape.length;
                if (!custom::dist_less(sc.pos, goal, shape.length / 2.f)) {
                    vec2 const d = custom::dir_vec(sc.pos, goal);
                    float const over = rw::distance(sc.pos, goal) - shape.length / 2.f;
                    sc.pos += d * over;
                    sc.vel += d * over;
                }
                sc.vel += rw::clamp_magnitude(goal - sc.pos, 10.f) / mathf::lerp(5.f, 1.5f, gill_rigor);
                sc.vel *= mathf::lerp(1.f, 0.8f, gill_rigor);
                connect_to_point(sc, pos, shape.length, true, 0.f, vec2{0.f, 0.f}, 0.f, 0.f);
                // AxolotlScale.Update
                sc.vel *= room.point_submerged(sc.pos) ? 0.5f : 0.9f;
                sc.last_pos = sc.pos;
                sc.pos += sc.vel;
            }
        }

        struct spine_data {
            vec2 pos, dir, perp;
            float rad;
        };
        // PlayerGraphics.SpinePosition: a point along the tail, s in [0, 1]
        spine_data spine_position(player::graphics_state const& g, float s, float alpha) {
            constexpr int n = 4;
            int i0 = int(std::floor(s * float(n) - 1.f));
            int i1 = std::min(int(std::floor(s * float(n))), n - 1);
            i0 = std::max(i0, 0);
            auto const at = [&](int i) { return rw::lerp(g.tail[i].last_pos, g.tail[i].pos, alpha); };
            vec2 const a0 = at(i0);
            vec2 const a2 = at(std::min(i1 + 1, n - 1));
            vec2 const a1 = at(i1);
            float const t = mathf::inverse_lerp(float(i0 + 1), float(i1 + 1), s * float(n));
            vec2 dir = rw::normalized(rw::lerp(a1 - a0, a2 - a1, t));
            if (dir == vec2{0.f, 0.f})
                dir = rw::normalized(g.tail[n - 1].pos - g.tail[n - 2].pos);
            float const r = mathf::lerp(g.tail[i0].rad * g.tail[i0].stretched, g.tail[i1].rad * g.tail[i1].stretched, t);
            return spine_data{rw::lerp(a0, a1, t), dir, custom::perpendicular_vector(dir), r};
        }
    }

    namespace {
        // SlugcatHand.EngageInMovement: true when the hand is free to do
        // something else (hold an item, or retract)
        bool engage_in_movement(sys::game_state& state, dcon::player_id p, player::graphics_state& g, int limb, room_view const& room) {
            auto const& w = state.world;
            auto& h = g.hands[limb];
            using player::limb_mode;
            using player::animation_index;
            using player::body_mode_index;
            vec2 const c0 = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
            vec2 const c1 = w.body_chunk_get_pos(player::body_chunk(state, p, 1));
            vec2 const v0 = w.body_chunk_get_vel(player::body_chunk(state, p, 0));
            auto const mode = w.player_get_body_mode(p);
            auto const anim = w.player_get_animation(p);
            int32_t const flip = w.player_get_flip_direction(p);
            float const frame = float(w.player_get_animation_frame(p));
            auto const& in = w.player_get_input(p)[0];
            float const side = limb == 0 ? -1.f : 1.f;
            bool flag = true;
            if (mode == body_mode_index::climbing_on_beam) {
                flag = false;
                if (anim == animation_index::climb_on_beam) {
                    bool const inner = (limb == 1) == (flip == 1);
                    float num = frame / 20.f * mathf::pi * 2.f;
                    num = inner ? std::sin(num) : std::cos(num);
                    h.mode = limb_mode::hunt_absolute_position;
                    h.absolute_hunt_pos = vec2{room_view::middle_of_tile(c0).x, c0.y};
                    h.absolute_hunt_pos.y += (inner ? -3.f : 3.f) + 6.f * num;
                    h.absolute_hunt_pos.x += inner ? float(-flip) : float(flip);
                    h.retract_counter = 40;
                } else if (anim == animation_index::hang_from_beam || anim == animation_index::get_up_on_beam) {
                    h.mode = limb_mode::hunt_absolute_position;
                    h.absolute_hunt_pos = vec2{c0.x, room_view::middle_of_tile(c0).y - 1.f};
                    h.absolute_hunt_pos.x += side * (10.f + 3.f * std::sin(6.2831855f * frame / 20.f));
                    auto const t = room.get_tile(h.absolute_hunt_pos);
                    if (t.solid() || !t.horizontal_beam())
                        h.absolute_hunt_pos.x = room_view::middle_of_tile(c0).x + side * 10.f;
                } else if (anim == animation_index::beam_tip || anim == animation_index::stand_on_beam) {
                    h.mode = limb_mode::hunt_relative_position;
                    h.relative_hunt_pos = vec2{-20.f + 40.f * float(limb), -4.f};
                    find_grip(h, room, c0, c0, 100.f, c0 + vec2{-10.f + 20.f * float(limb), ((limb == 0) == (flip == -1)) ? 0.f : -5.f},
                        limb == 0 ? 1 : -1, -1);
                    h.hunt_speed = 5.f;
                    h.quickness = 0.2f;
                } else if (anim == animation_index::hang_under_vertical_beam) {
                    h.mode = limb_mode::hunt_absolute_position;
                    h.absolute_hunt_pos = room_view::middle_of_tile(c0) + vec2{limb == 0 ? -0.5f : 0.5f, limb == 0 ? 20.f : 25.f};
                    h.hunt_speed = 10.f;
                    h.quickness = 1.f;
                }
            } else if (mode == body_mode_index::crawl) {
                flag = false;
                h.mode = limb_mode::hunt_absolute_position;
                h.hunt_speed = 12.f;
                h.quickness = 0.7f;
                auto const& h0 = g.hands[0];
                if ((limb == 0 || (std::fabs(h0.pos.x - c0.x) < 10.f && h0.reached_snap_position)) && !custom::dist_less(c0, h.absolute_hunt_pos, 29.f)) {
                    vec2 const from = c0 + vec2{float(flip) * 20.f, 0.f};
                    find_grip(h, room, from, from, 100.f, vec2{c0.x + float(flip) * 28.f, room_view::middle_of_tile(c0).y - 10.f}, 2, 1);
                }
            } else if (mode == body_mode_index::corridor_climb) {
                flag = false;
                h.mode = limb_mode::hunt_absolute_position;
                if (!custom::dist_less(h.pos, c0, 20.f)) {
                    vec2 const v = custom::dir_vec(c1, c0);
                    vec2 const goal = c0 + rw::normalized(v + rw::normalized(vec2{float(in.x), float(in.y)}) * 1.5f) * 20.f
                        + custom::perpendicular_vector(v) * (6.f - 12.f * float(limb));
                    find_grip(h, room, c0, c0, 100.f, goal, 2, 2);
                }
            } else if (mode == body_mode_index::wall_climb) {
                flag = false;
                h.mode = limb_mode::hunt_absolute_position;
                h.absolute_hunt_pos.x = room_view::middle_of_tile(c0).x + float(flip) * 10.f;
                h.absolute_hunt_pos.y = ((limb == 0) == (flip == -1)) ? c0.y - 7.f : c0.y + 3.f;
            } else if (mode == body_mode_index::swimming) {
                flag = false;
                float const cycle = w.player_get_swim_cycle(p);
                float const num4 = cycle < 3.f ? cycle / 3.f : 1.f - (cycle - 3.f);
                if (anim == animation_index::deep_swim) {
                    h.mode = limb_mode::hunt_relative_position;
                    float const num5 = mathf::pow(1.f - mathf::inverse_lerp(0.5f, 1.f, num4), 1.5f);
                    h.relative_hunt_pos = custom::deg_to_vec((20.f + num5 * 140.f) * side) * 20.f;
                    if (cycle < 3.f)
                        h.relative_hunt_pos.x *= 0.5f;
                } else {
                    h.mode = limb_mode::hunt_absolute_position;
                    h.absolute_hunt_pos.x = c0.x + mathf::lerp(side * 30.f, float(in.x) * 10.f * -std::sin(num4 * 2.f * mathf::pi), 0.5f);
                    h.absolute_hunt_pos.y = room.get_float_water_level(h.absolute_hunt_pos) - 7.f;
                    h.absolute_hunt_pos += custom::deg_to_vec(360.f * num4 * (in.x == 0 ? 1.f : -float(in.x))) * side * (in.x == 0 ? 5.f : 10.f);
                }
                h.hunt_speed = 5.f;
                h.quickness = 0.5f;
            } else if (mode == body_mode_index::default_) {
                if (g.airborne_counter > 180.f && !custom::dist_less(vec2{0.f, 0.f}, v0, 4.f)) {
                    // flailing for something to hold on to
                    flag = false;
                    h.retract_counter = 0;
                    vec2 const b = custom::deg_to_vec(g.random.value() * 360.f) * 40.f * g.random.value();
                    vec2 const probe = c0 + v0 * 4.f + b;
                    if (room.get_tile(probe).t != terrain::air) {
                        h.mode = limb_mode::hunt_absolute_position;
                        h.absolute_hunt_pos = probe;
                        h.hunt_speed = 18.f;
                        h.quickness = 1.f;
                    } else {
                        bool const upright = c0.y > c1.y + 5.f;
                        h.mode = limb_mode::hunt_relative_position;
                        h.relative_hunt_pos.x = (std::fabs(v0.x * (upright ? 2.1f : 1.f)) + 4.f) * (-1.f + 2.f * float(limb));
                        h.relative_hunt_pos.y = v0.y * (upright ? -3.f : -0.9f) + std::fabs(v0.x * 0.6f) + 1.f;
                        if (rw::magnitude(v0) > 10.f)
                            h.relative_hunt_pos += custom::deg_to_vec(g.random.value() * 360.f) * g.random.value() * g.random.value() * rw::magnitude(v0);
                        h.hunt_speed = 8.f;
                        h.quickness = 0.6f;
                    }
                }
            }
            if (anim == animation_index::down_on_fours || anim == animation_index::crawl_turn) {
                flag = false;
                h.mode = limb_mode::hunt_absolute_position;
                vec2 const cv = rw::normalized(v0);
                h.absolute_hunt_pos = c0 + vec2{-6.f + 12.f * float(limb) + cv.x * 20.f, std::fabs(cv.y) * -20.f};
            } else if (anim == animation_index::ledge_grab) {
                flag = false;
                h.absolute_hunt_pos = room_view::middle_of_tile(c0);
                if ((limb == 0) == (flip == -1))
                    h.absolute_hunt_pos += vec2{float(flip) * 10.f, -10.f};
                else
                    h.absolute_hunt_pos += vec2{float(flip) * 15.f, 10.f};
            } else if (anim == animation_index::vine_grab && rot::vines::valid(state, room.id, w.player_get_vine(p))) {
                // hand over hand along the vine
                flag = false;
                rot::vines::position const vp{w.player_get_vine(p), w.player_get_vine_float_pos(p)};
                float const along = 20.f * side * std::sin(frame / 30.f * 2.f * mathf::pi) / rot::vines::total_length(state, vp.vine);
                h.mode = limb_mode::hunt_absolute_position;
                h.absolute_hunt_pos = rot::vines::on_vine_pos(state, rot::vines::position{vp.vine, vp.float_pos + along})
                    + custom::perpendicular_vector(rot::vines::vine_dir(state, vp)) * rot::vines::vine_rad(state, vp) * side;
            }
            return flag;
        }

        // SlugcatHand.Update
        void hand_update(sys::game_state& state, dcon::player_id p, player::graphics_state& g, int limb, room_view const& room) {
            auto const& w = state.world;
            auto& h = g.hands[limb];
            using player::limb_mode;
            auto const b0 = player::body_chunk(state, p, 0);
            vec2 const c0 = w.body_chunk_get_pos(b0);
            vec2 const v0 = w.body_chunk_get_vel(b0);
            body_parts::limb_update(h, hand, c0, v0, w.body_chunk_get_pos(player::body_chunk(state, p, 1)), room);
            body_parts::as_body_part(h, hand.rad, [&](player::body_part& bp) { connect_to_point(bp, c0, 20.f, false, 0.f, v0, 0.f, 0.f); });
            bool flag;
            if (h.reaching_for_object) {
                h.mode = limb_mode::hunt_absolute_position;
                flag = false;
                h.reaching_for_object = 0;
            } else {
                flag = engage_in_movement(state, p, g, limb, room);
            }
            // HeavyCarry (a cicada): the first hand always holds on to it, the
            // other one unless it is busy climbing (no Remix)
            auto const carried = cicada::held_by(state, p);
            if (limb == 0 && carried)
                flag = true;
            if (flag && carried) {
                auto const gc = cicada::chunk(state, carried, creature::grasp_chunk(w, player::creature_of(state, p), 0));
                vec2 const gp = w.body_chunk_get_pos(gc);
                h.mode = limb_mode::hunt_absolute_position;
                h.absolute_hunt_pos = gp + custom::perpendicular_vector(c0 - gp) * w.body_chunk_get_rad(gc) * 0.8f * (limb == 0 ? -1.f : 1.f);
                h.hunt_speed = 20.f;
                h.quickness = 1.f;
                flag = false;
            }
            if (flag && items::held(state, p, uint32_t(limb))) {
                // holding something: out to the side, spears swinging along while walking
                h.mode = limb_mode::hunt_relative_position;
                h.relative_hunt_pos = vec2{-20.f + 40.f * float(limb), -12.f};
                auto const& in = w.player_get_input(p)[0];
                if (g.spear_dir != 0.f && w.player_get_body_mode(p) == player::body_mode_index::stand) {
                    float const frame = float(w.player_get_animation_frame(p));
                    vec2 b = custom::deg_to_vec(180.f + (limb == 0 ? -1.f : 1.f) * 8.f + float(in.x) * 4.f) * 12.f;
                    b.y += std::sin(frame / 6.f * 2.f * mathf::pi) * 2.f;
                    b.x -= std::cos(frame / 12.f * 2.f * mathf::pi) * 4.f * float(in.x);
                    b.x += float(in.x) * 2.f;
                    h.relative_hunt_pos = rw::lerp(h.relative_hunt_pos, b, std::fabs(g.spear_dir));
                }
                flag = false;
            }
            if (flag && h.mode != limb_mode::retracted) {
                if (++h.retract_counter > 5) {
                    h.mode = limb_mode::hunt_absolute_position;
                    h.pos = rw::lerp(h.pos, c0, mathf::clamp((float(h.retract_counter) - 5.f) * 0.05f, 0.f, 1.f));
                    if (custom::dist_less(h.pos, c0, 2.f) && h.reached_snap_position)
                        h.mode = limb_mode::retracted;
                    h.absolute_hunt_pos = c0;
                    h.hunt_speed = 1.f + float(h.retract_counter) * 0.2f;
                    h.quickness = 1.f;
                }
            } else {
                h.retract_counter = std::max(0, h.retract_counter - 10);
            }
        }
    }

    void set_weaver(sys::game_state& state, dcon::player_id p, bool on) {
        state.world.player_get_graphics(p).weaver = on ? 1 : 0;
    }

    bool weaver(sys::game_state const& state, dcon::player_id p) {
        return state.world.player_get_graphics(p).weaver != 0;
    }

    void throw_object(sys::game_state& state, dcon::player_id p, uint32_t hand, dcon::item_id item) {
        auto& g = state.world.player_get_graphics(p);
        g.throw_counter = 5;
        g.thrown_item = item ? uint32_t(item.index()) + 1 : 0;
        g.throw_hand = uint8_t(hand);
    }

    rw::vec2 hand_position(sys::game_state const& state, dcon::player_id p, uint32_t hand) {
        auto const& g = state.world.player_get_graphics(p);
        return g.hands[hand & 1].pos;
    }

    void reset(sys::game_state& state, dcon::player_id p) {
        auto& w = state.world;
        auto& g = w.player_get_graphics(p);
        vec2 const c0 = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
        vec2 const c1 = w.body_chunk_get_pos(player::body_chunk(state, p, 1));
        uint8_t const weaver = g.weaver; // a look, survives resets
        g = player::graphics_state{};
        g.draw_pos[0] = g.last_draw_pos[0] = c0;
        g.draw_pos[1] = g.last_draw_pos[1] = c1;
        auto const place = [](player::body_part& bp, vec2 at, float rad) {
            bp.pos = bp.last_pos = at;
            bp.vel = vec2{0.f, 0.f};
            bp.rad = rad;
            bp.stretched = 1.f;
        };
        place(g.head, c0, head_rad);
        place(g.legs, c1, legs_rad);
        for (int i = 0; i < 4; ++i)
            place(g.tail[i], c1 - vec2{0.f, float(i + 1) * 4.f}, tail_rad[i]);
        for (auto& gl : g.gills)
            place(gl, c0, 1.f);
        for (auto& h : g.hands) {
            h = player::hand_state{};
            h.pos = h.last_pos = h.absolute_hunt_pos = c0;
        }
        g.weaver = weaver;
        g.initialized = 1;
    }

    void update(sys::game_state& state, dcon::player_id p, room_view const& room) {
        auto& w = state.world;
        auto& g = w.player_get_graphics(p);
        if (!g.initialized)
            reset(state, p);
        auto const b0 = player::body_chunk(state, p, 0);
        auto const b1 = player::body_chunk(state, p, 1);
        vec2 const c0 = w.body_chunk_get_pos(b0);
        vec2 const c1 = w.body_chunk_get_pos(b1);
        vec2 const v0 = w.body_chunk_get_vel(b0);
        vec2 const v1 = w.body_chunk_get_vel(b1);
        auto const mode = w.player_get_body_mode(p);
        auto const anim = w.player_get_animation(p);
        int32_t const flip = w.player_get_flip_direction(p);
        float const frame = float(w.player_get_animation_frame(p));
        float const sub1 = physics::submersion(state, room, b1);
        float const gravity = room.gravity;

        for (int i = 0; i < 2; ++i)
            g.last_draw_pos[i] = g.draw_pos[i];
        g.draw_pos[0] = c0;
        g.draw_pos[1] = c1;

        // per body mode bobbing of the draw positions
        bool running = false;
        float tail_stiffness = 1.f;
        if (mode == player::body_mode_index::stand) {
            g.draw_pos[0].x += float(flip) * 6.f * mathf::clamp(std::fabs(v1.x) - 0.2f, 0.f, 1.f);
            g.draw_pos[0].y += std::cos(frame / 6.f * 2.f * mathf::pi) * 2.f;
            g.draw_pos[1].x -= float(flip) * (1.5f - frame / 6.f);
            g.draw_pos[1].y += 2.f + std::sin(frame / 6.f * 2.f * mathf::pi) * 4.f;
            running = std::fabs(v0.x) > 2.f && std::fabs(v1.x) > 2.f;
            tail_stiffness = 1.f - mathf::clamp((std::fabs(v1.x) - 1.f) * 0.5f, 0.f, 1.f);
        } else if (mode == player::body_mode_index::crawl) {
            float const s = std::sin(frame / 21.f * 2.f * mathf::pi);
            float const c = std::cos(frame / 14.f * 2.f * mathf::pi);
            float const launch = w.player_get_super_launch_jump(p) > 19 ? 0.f : 1.f;
            g.draw_pos[0].x += c * float(flip) * 2.f;
            g.draw_pos[0].y -= s * -1.5f - 3.f;
            g.head.vel.y -= s * -0.5f - 0.5f;
            g.head.vel.x += c0.x < c1.x ? -1.f : 1.f;
            g.draw_pos[1].x += -3.f * s * float(flip);
            g.draw_pos[1].y -= c * 1.5f - 7.f + 3.f * launch;
        } else if (mode == player::body_mode_index::climbing_on_beam && anim == player::animation_index::stand_on_beam) {
            g.draw_pos[1].y += 3.f;
        }

        // tail
        vec2 pull = c0;
        if (running) {
            pull = c1;
            pull.y -= 4.f;
            pull.x += float(flip) * 16.f * mathf::clamp(std::fabs(v1.x) - 0.2f, 0.f, 1.f);
        }
        vec2 prev = c1;
        float force = 28.f;
        for (int l = 0; l < 4; ++l) {
            auto& t = g.tail[l];
            tail_update(g, l, g.draw_pos[1], room);
            t.vel *= mathf::lerp(0.75f, 0.95f, tail_stiffness * (1.f - sub1));
            t.vel.y -= mathf::lerp(0.1f, 0.5f, tail_stiffness) * (1.f - sub1) * gravity;
            tail_stiffness = (tail_stiffness * 10.f + 1.f) / 11.f;
            float const max_d = 9.f * float(l + 1);
            if (!custom::dist_less(t.pos, c1, max_d))
                t.pos = c1 + custom::dir_vec(c1, t.pos) * max_d;
            float const d = rw::distance(pull, t.pos);
            if (d > 0.f)
                t.vel += custom::dir_vec(pull, t.pos) * force / d;
            force *= 0.5f;
            pull = prev;
            prev = t.pos;
        }

        // head
        vec2 offset = custom::dir_vec(g.draw_pos[1], g.draw_pos[0]) * 3.f;
        if (mode == player::body_mode_index::crawl)
            offset.x *= 2.5f;
        else if (mode == player::body_mode_index::corridor_climb && offset.y < 0.f)
            offset.y *= 2.f;
        g.head.vel += g.look_direction;
        generic_update(g.head, room, head_air_friction, head_surface_friction);
        connect_to_point(g.head, rw::lerp(g.draw_pos[0], g.draw_pos[1], 0.2f) + offset,
            anim == player::animation_index::hang_from_beam ? 0.f : 3.f, false, 0.2f, v0, 0.7f, 0.1f);

        // legs
        generic_update(g.legs, room, legs_air_friction, legs_surface_friction);
        if (mode == player::body_mode_index::corridor_climb) {
            connect_to_point(g.legs, c1 + custom::dir_vec(c0, c1) * 4.f, 2.f, false, 0.25f, v1, 0.5f, 0.1f);
            g.legs_direction += custom::deg_to_vec(aim(c0, c1));
        } else if (w.body_chunk_get_contact_point(b1).y == -1 || anim == player::animation_index::stand_on_beam) {
            connect_to_point(g.legs, c1 + vec2{g.legs_direction.x * 8.f, 1.f}, 5.f, false, 0.25f, vec2{v1.x, -10.f}, 0.5f, 0.1f);
            g.legs_direction.x -= float(w.body_chunk_get_on_slope(b1));
            g.legs_direction.y -= 1.f;
        } else if (anim == player::animation_index::beam_tip) {
            connect_to_point(g.legs, c1 + vec2{0.f, -8.f}, 0.f, false, 0.25f, vec2{0.f, -10.f}, 0.5f, 0.1f);
            g.legs_direction += custom::dir_vec(g.draw_pos[0], room_view::middle_of_tile(c1) + vec2{0.f, -10.f});
        } else if (anim == player::animation_index::climb_on_beam) {
            vec2 const b{float(-flip) * (5.f - std::sin(frame / 20.f * mathf::pi * 2.f)), -16.f - 5.f * std::cos(frame / 20.f * mathf::pi * 2.f)};
            connect_to_point(g.legs, c0 + b, 0.f, false, 0.25f, vec2{0.f, 0.f}, 0.5f, 0.1f);
            g.legs_direction.y -= 1.f;
        } else {
            connect_to_point(g.legs, c1 + vec2{g.legs_direction.x * 8.f, anim == player::animation_index::hang_from_beam ? -5.f : -2.f},
                4.f, false, 0.25f, vec2{v1.x, -10.f}, 0.5f, 0.1f);
            g.legs_direction += v1 * 0.01f;
            g.legs_direction.y -= 0.05f;
        }
        g.legs_direction = rw::normalized(g.legs_direction);

        if (w.player_get_stats(p).name == player::slugcat_name::rivulet)
            gills_update(g, c0, c1, room);

        // spearDir: where a held spear points while walking
        int32_t const in_x = w.player_get_input(p)[0].x;
        if (mode == player::body_mode_index::stand && in_x != 0)
            g.spear_dir = mathf::clamp(g.spear_dir + float(in_x) * 0.1f, -1.f, 1.f);
        else if (g.spear_dir < 0.f)
            g.spear_dir = mathf::min(g.spear_dir + 0.05f, 0.f);
        else if (g.spear_dir > 0.f)
            g.spear_dir = mathf::max(g.spear_dir - 0.05f, 0.f);
        if (mode == player::body_mode_index::default_ && anim == player::animation_index::none
        && w.body_chunk_get_contact_point(b0) == ivec2{0, 0} && w.body_chunk_get_contact_point(b1) == ivec2{0, 0})
            g.airborne_counter += rw::magnitude(v0);
        else
            g.airborne_counter = 0.f;

        // hands: following a throw, or reaching over the shoulder for the back spear
        if (consious(state, p)) {
            dcon::item_id const thrown = g.thrown_item ? dcon::item_id{dcon::item_id::value_base_t(g.thrown_item - 1)} : dcon::item_id{};
            int32_t const back_counter = w.player_get_back_counter(p);
            if (g.throw_counter > 0 && thrown && w.item_is_valid(thrown)) {
                auto& h = g.hands[g.throw_hand & 1];
                vec2 const ip = w.body_chunk_get_pos(items::chunk(state, thrown));
                h.reaching_for_object = 1;
                h.absolute_hunt_pos = ip;
                if (custom::dist_less(h.pos, ip, 40.f))
                    h.pos = ip;
                else
                    h.vel += custom::dir_vec(h.pos, ip) * 6.f;
                g.hands[1 - (g.throw_hand & 1)].vel -= custom::dir_vec(h.pos, ip) * 3.f;
                --g.throw_counter;
            } else if (w.player_get_stats(p).name == player::slugcat_name::red && back_counter > 5) {
                bool const has = bool(w.player_get_back_spear(p));
                int hand = -1;
                for (int i = 0; i < 2 && hand == -1; ++i) {
                    auto const held = items::held(state, p, uint32_t(i));
                    if ((has && !held) || (!has && held && w.item_get_type(held) == world::item_type::spear))
                        hand = i;
                }
                if (hand > -1) {
                    auto& h = g.hands[hand];
                    float const sd = hand == 0 ? -1.f : 1.f;
                    h.reaching_for_object = 1;
                    h.mode = player::limb_mode::hunt_relative_position;
                    float const k = float(back_counter);
                    if (has)
                        h.relative_hunt_pos = slerp(vec2{sd * 20.f, -30.f} * std::sin(mathf::inverse_lerp(9.f, 20.f, k) * mathf::pi), vec2{0.f, 1.f},
                            mathf::inverse_lerp(9.f, 20.f, k));
                    else
                        h.relative_hunt_pos = slerp(vec2{sd * 30.f, -20.f} * mathf::lerp(1.f, 0.2f, std::fabs(w.player_get_back_flip(p))), vec2{1.f, 1.f},
                            mathf::inverse_lerp(14.f, 20.f, k));
                    g.draw_pos[0] += custom::dir_vec(h.absolute_hunt_pos, g.draw_pos[0]) * 0.7f;
                    g.head.vel += custom::dir_vec(h.absolute_hunt_pos, g.head.pos) * 1.5f;
                }
            }
        }
        for (int i = 0; i < 2; ++i) {
            hand_update(state, p, g, i, room);
            if (!std::isfinite(g.hands[i].pos.x) || !std::isfinite(g.hands[i].pos.y))
                g.hands[i].pos = c0;
        }

        // Spearmaster shuts its eyes while pulling a needle out
        if (w.player_get_stats(p).name == player::slugcat_name::spear && w.player_get_spear_prog(p) > 0.1f)
            g.blink = 5;
        if (g.random.value() < 0.1f)
            g.halo_flux = mathf::lerp(0.6f, 1.f, g.random.value());
        ++g.ticks;

        // blinking
        g.last_look_direction = g.look_direction;
        --g.blink;
        if (g.blink < -g.random.range(2, 1800))
            g.blink = g.random.range(3, g.random.range(3, 10));
    }

    void build(sys::game_state const& state, dcon::player_id p, float alpha, std::vector<draw_cmd>& out) {
        auto const& w = state.world;
        auto const& g = w.player_get_graphics(p);
        if (!g.initialized)
            return;
        rw::vec3 const body_color = w.player_get_stats(p).body_color;
        bool const decorated = g.weaver && w.player_get_stats(p).name == player::slugcat_name::watcher;
        // RainWorld.GoldRGB; the decorated Watcher's eyes are SaturatedGold
        rw::vec3 const gold{0.529f, 0.365f, 0.184f};
        rw::vec3 const eye_color = decorated ? rw::vec3{1.f, 0.73f, 0.368f} : w.player_get_stats(p).eye_color;
        auto const mode = w.player_get_body_mode(p);
        auto const anim = w.player_get_animation(p);
        int32_t const flip = w.player_get_flip_direction(p);
        int32_t const frame = w.player_get_animation_frame(p);
        bool const awake = consious(state, p);
        auto const lerp_part = [&](player::body_part const& bp) { return rw::lerp(bp.last_pos, bp.pos, alpha); };
        vec2 const v = rw::lerp(g.last_draw_pos[0], g.draw_pos[0], alpha);  // upper body
        vec2 const v2 = rw::lerp(g.last_draw_pos[1], g.draw_pos[1], alpha); // lower body
        vec2 v3 = lerp_part(g.head);
        auto const push = [&](std::string_view element, vec2 pos, float rot, vec2 scale, vec2 anchor, rw::vec3 color) {
            draw_cmd c;
            c.spr = sprite{element, pos, rot, scale, anchor, color};
            out.push_back(c);
        };

        auto const name = w.player_get_stats(p).name;
        // Gourmand is wide, Spearmaster thin (body, hips and head scaleX)
        float body_width = 1.f, hips_width = 1.f, head_width = 1.f;
        if (name == player::slugcat_name::gourmand) {
            body_width = 1.4f;
            hips_width = 1.6f;
        } else if (name == player::slugcat_name::spear) {
            body_width = 0.76f;
            hips_width = 0.76f;
            head_width = 0.85f;
        }

        // 0 body, 1 hips
        push("BodyA", v, aim(v2, v), vec2{body_width, 1.f}, vec2{0.5f, 0.7894737f}, body_color);
        push("HipsA", (v2 * 2.f + v) / 3.f, aim(v, lerp_part(g.tail[0])), vec2{hips_width, 1.f}, vec2{0.5f, 0.5f}, body_color);

        // 2 tail TriangleMesh (15 vertices, 13 triangles)
        {
            vec2 verts[15];
            vec2 base = (v2 * 3.f + v) / 4.f;
            float width = 6.f;
            for (int i = 0; i < 4; ++i) {
                vec2 const sp = lerp_part(g.tail[i]);
                vec2 const n = rw::normalized(sp - base);
                vec2 const a = custom::perpendicular_vector(n);
                float const d3 = i == 0 ? 0.f : rw::distance(sp, base) / 5.f;
                verts[i * 4] = base - a * width + n * d3;
                verts[i * 4 + 1] = base + a * width + n * d3;
                float const r = g.tail[i].rad * g.tail[i].stretched;
                if (i < 3) {
                    verts[i * 4 + 2] = sp - a * r - n * d3;
                    verts[i * 4 + 3] = sp + a * r - n * d3;
                } else {
                    verts[i * 4 + 2] = sp;
                }
                width = r;
                base = sp;
            }
            static constexpr int tris[13][3] = {
                {0, 1, 2}, {1, 2, 3}, {4, 5, 6}, {5, 6, 7}, {8, 9, 10}, {9, 10, 11}, {12, 13, 14},
                {2, 3, 4}, {3, 4, 5}, {6, 7, 8}, {7, 8, 9}, {10, 11, 12}, {11, 12, 13},
            };
            for (auto const& t : tris) {
                draw_cmd c;
                c.is_triangle = true;
                c.tri[0] = verts[t[0]];
                c.tri[1] = verts[t[1]];
                c.tri[2] = verts[t[2]];
                c.color = body_color;
                out.push_back(c);
            }
        }

        // head direction picks one of the 18 rotated head elements
        float head_rot = aim(rw::lerp(v2, v, 0.5f), v3);
        int32_t head_index = int32_t(std::round(std::fabs(head_rot / 360.f * 34.f)));
        vec2 look = rw::lerp(g.last_look_direction, g.look_direction, alpha) * 3.f;
        std::string_view face_element;
        float face_rot = 0.f;
        float face_scale_x = 1.f;
        auto const default_face = [&](int idx) -> std::string_view {
            idx = custom::int_clamp(idx, 0, 8);
            return g.blink <= 0 ? std::string_view{face_a_names[size_t(idx)]} : std::string_view{face_b_names[size_t(idx)]};
        };
        if (awake) {
            if ((mode == player::body_mode_index::stand && w.player_get_input(p)[0].x != 0) || mode == player::body_mode_index::crawl) {
                if (mode == player::body_mode_index::crawl) {
                    head_index = 7;
                    face_scale_x = mathf::sign(v.x - v2.x);
                } else {
                    head_index = 6;
                    face_scale_x = head_rot < 0.f ? -1.f : 1.f;
                }
                look.x = 0.f;
                face_element = default_face(4);
            } else {
                vec2 d = v3 - v2;
                d.x *= 1.f - rw::magnitude(look) / 3.f;
                d = rw::normalized(d);
                face_scale_x = std::fabs(look.x) < 0.1f ? (head_rot < 0.f ? -1.f : 1.f) : mathf::sign(look.x);
                face_element = default_face(int32_t(std::round(std::fabs(aim(vec2{0.f, 0.f}, d) / 22.5f))));
            }
        } else {
            look = vec2{0.f, 0.f};
            head_index = 0;
            face_element = player::dead(state, p) ? "FaceDead" : "FaceStunned";
            face_rot = head_rot;
        }
        head_index = custom::int_clamp(head_index, 0, 17);

        // 3 head
        push(head_names[size_t(head_index)], v3, head_rot, vec2{(head_rot < 0.f ? -1.f : 1.f) * head_width, 1.f}, vec2{0.5f, 0.5f}, body_color);

        // 4 legs
        {
            vec2 legs_pos = lerp_part(g.legs);
            float scale_x = 1.f;
            std::string_view element = "LegsAAir0";
            bool visible = true;
            float const facing = flip > 0 ? 1.f : -1.f;
            if (mode == player::body_mode_index::stand) {
                element = legs_names[size_t(custom::int_clamp(frame, 0, 6))];
                scale_x = facing;
            } else if (mode == player::body_mode_index::crawl) {
                element = legs_crawling_names[size_t(custom::int_clamp(frame / 2, 0, 5))];
                scale_x = facing;
            } else if (mode == player::body_mode_index::corridor_climb) {
                int32_t f = frame;
                if (f > 6) {
                    f %= 6;
                    scale_x = -1.f;
                }
                element = legs_climbing_names[size_t(custom::int_clamp(f, 0, 6))];
            } else if (mode == player::body_mode_index::climbing_on_beam) {
                if (anim == player::animation_index::stand_on_beam) {
                    element = legs_on_pole_names[size_t(frame < 7 ? frame : 0)];
                    scale_x = facing;
                } else if (anim == player::animation_index::beam_tip) {
                    element = "LegsAPole";
                } else if (anim == player::animation_index::climb_on_beam) {
                    element = "LegsAVerticalPole";
                    scale_x = facing;
                    legs_pos.y = mathf::clamp(legs_pos.y, v2.y - 6.f, v2.y + 4.f);
                }
            } else if (mode == player::body_mode_index::wall_climb) {
                element = "LegsAWall";
                scale_x = facing;
            } else if (mode == player::body_mode_index::default_) {
                if (anim == player::animation_index::ledge_grab) {
                    element = "LegsAWall";
                    scale_x = facing;
                }
            } else if (mode == player::body_mode_index::swimming && anim == player::animation_index::deep_swim) {
                visible = false;
            }
            if (visible)
                push(element, legs_pos, aim(g.legs_direction, vec2{0.f, 0.f}), vec2{scale_x, 1.f}, vec2{0.5f, 0.25f}, body_color);
        }

        // 5, 6 arms and 7, 8 hands gripping terrain
        for (int j = 0; j < 2; ++j) {
            auto const& h = g.hands[j];
            vec2 const hp = rw::lerp(h.last_pos, h.pos, alpha);
            bool const snapped_grip = (anim == player::animation_index::climb_on_beam || anim == player::animation_index::hang_from_beam
                || anim == player::animation_index::get_up_on_beam) && h.reached_snap_position;
            if (h.mode != player::limb_mode::retracted && (anim != player::animation_index::climb_on_beam || !h.reached_snap_position)) {
                float shoulder = 4.5f / (float(h.retract_counter) + 1.f);
                if (anim == player::animation_index::hang_from_beam)
                    shoulder *= 0.5f;
                shoulder *= std::fabs(std::cos(aim(v2, v) / 360.f * mathf::pi * 2.f));
                if (name == player::slugcat_name::spear)
                    shoulder *= 0.6f;
                vec2 const sp = v + rotate_around_origo(vec2{(-1.f + 2.f * float(j)) * shoulder, -3.5f}, aim(v2, v));
                size_t const idx = size_t(custom::int_clamp(int32_t(std::round(mathf::clamp(rw::distance(hp, sp) / 2.f, 0.f, 12.f))), 0, 12));
                float scale_y;
                if (mode == player::body_mode_index::crawl)
                    scale_y = v.x < v2.x ? -1.f : 1.f;
                else if (mode == player::body_mode_index::wall_climb)
                    scale_y = flip == -1 ? -1.f : 1.f;
                else {
                    // Custom.DistanceToLine(hand, body top, body bottom)
                    float const dl = ((v.y - v2.y) * hp.x - (v.x - v2.x) * hp.y + v.x * v2.y - v.y * v2.x);
                    scale_y = mathf::sign(dl);
                }
                if (anim == player::animation_index::hang_under_vertical_beam)
                    scale_y = j == 0 ? 1.f : -1.f;
                push(arm_names[idx], hp, aim(hp, sp) + 90.f, vec2{1.f, scale_y}, vec2{0.9f, 0.5f}, body_color);
            }
            if (snapped_grip) {
                vec2 const at = hp + vec2{0.f, anim != player::animation_index::climb_on_beam ? 3.f : 0.f};
                push("OnTopOfTerrainHand", at, 0.f, vec2{j == 1 ? -1.f : 1.f, 1.f}, vec2{0.5f, 0.5f}, body_color);
            }
        }

        // 9 face
        vec2 const face_pos = v3 + look + vec2{0.f, -2.f};
        push(face_element, face_pos, face_rot, vec2{face_scale_x, 1.f}, vec2{0.5f, 0.5f}, eye_color);

        // 12 Artificer: the mushroom (scar) by one eye, always on the same side
        if (name == player::slugcat_name::artificer) {
            vec2 at = face_pos + vec2{3.f, 3.f};
            if (anim == player::animation_index::flip)
                at = face_pos + custom::deg_to_vec(face_rot) * 4.f;
            push("MushroomA", at, face_rot, vec2{1.f, 1.f}, vec2{0.5f, 0.5f}, rw::vec3{0.27059f, 0.15686f, 0.23529f});
        }

        // Rivulet: AxolotlGills, the base scales then their coloured tips
        if (name == player::slugcat_name::rivulet) {
            rw::vec3 const effect{0.87451f, 0.17647f, 0.91765f};
            for (int layer = 0; layer < 2; ++layer) {
                for (int i = 5; i >= 0; --i) {
                    auto const shape = gill(i);
                    bool const left = i < 3;
                    vec2 const at = face_pos + vec2{left ? -5.f : 5.f, 0.f};
                    // every scale trails to the left; vanilla turns the right
                    // side's sprites around to mirror them
                    float const rot = aim(at, lerp_part(g.gills[i])) + (left ? 0.f : 180.f);
                    // the packed scale sprite keeps only the drawn part of
                    // LizardScaleA3 (18px above its anchor)
                    float sx = shape.width * (left ? -1.f : 1.f);
                    push(layer == 0 ? "LizardScaleA3" : "LizardScaleB3", at, rot, vec2{sx, shape.length / 18.f}, vec2{0.5f, 0.5f},
                        layer == 0 ? body_color : effect);
                }
            }
        }

        // Spearmaster: TailSpeckles, 5 rows of 3 dots down the tail, one of
        // them growing a needle while it pulls a spear out
        if (name == player::slugcat_name::spear) {
            constexpr int rows = 5, lines = 3;
            float const prog = w.player_get_spear_prog(p);
            int const spear_row = w.player_get_spear_row(p);
            int const spear_line = w.player_get_spear_line(p);
            for (int i = 0; i < rows; ++i) {
                float const f = mathf::inverse_lerp(0.f, float(rows - 1), float(i));
                auto const sd = spine_position(g, mathf::lerp(0.4f, 0.95f, mathf::pow(f, 0.8f)), alpha);
                float const k = 0.8f * mathf::pow(f, 0.5f);
                float const k2 = (0.8f - k) * prog;
                rw::vec3 const pale = rw::vec3{1.f, 1.f, 1.f} + (body_color - rw::vec3{1.f, 1.f, 1.f}) * 0.3f;
                rw::vec3 const col = body_color + (pale - body_color) * mathf::clamp01(0.2f + k + k2);
                for (int j = 0; j < lines; ++j) {
                    float t = (float(j) + (i % 2 != 0 ? 0.f : 0.5f)) / float(lines - 1);
                    t = -1.f + 2.f * t;
                    if (t < -1.f)
                        t += 2.f;
                    else if (t > 1.f)
                        t -= 2.f;
                    t = mathf::sign(t) * mathf::pow(std::fabs(t), 0.6f);
                    vec2 const at = sd.pos + sd.perp * (sd.rad + 0.5f) * t;
                    vec2 scale{custom::lerp_map(std::fabs(t), 0.4f, 1.f, 1.f, 0.f), 1.f};
                    if (prog > 0.f) {
                        if (i == spear_row && j == spear_line)
                            scale *= 1.f + prog * 2.f;
                        else if ((std::abs(i - spear_row) == 1 && j == spear_line) || (i == spear_row && std::abs(j - spear_line) == 1))
                            scale *= 1.f + prog;
                    }
                    push("tinyStar", at, aim(vec2{0.f, 0.f}, sd.dir), scale, vec2{0.5f, 0.5f}, col);
                    if (i == spear_row && j == spear_line && prog > 0.f) {
                        // the BioSpear growing out of it (drawn as a shaft)
                        vec2 side = custom::perpendicular_vector(sd.dir);
                        if (side.y > 0.35f)
                            side = -side;
                        draw_cmd c;
                        c.is_triangle = true;
                        vec2 const tip = at + side * (prog * 0.5f * 44.f);
                        vec2 const n = custom::perpendicular_vector(side) * 1.f;
                        c.tri[0] = at + n;
                        c.tri[1] = at - n;
                        c.tri[2] = tip;
                        c.color = rw::vec3{1.f, 1.f, 1.f};
                        out.push_back(c);
                    }
                }
            }
        }

        // Watcher: WeaverParts, feathers down the tail and a flickering halo
        if (decorated) {
            constexpr int rows = 4, lines = 3;
            auto const side_pos = [](int i, int j) {
                float num = (float(j) + (i % 2 != 0 ? 0.f : 0.5f)) / float(lines - 1);
                num = -1.f + 2.f * num;
                if (num < -1.f)
                    num += 2.f;
                else if (num > 1.f)
                    num -= 2.f;
                return mathf::sign(num) * mathf::pow(std::fabs(num), 0.6f);
            };
            rw::vec4 const tip{gold.x * 1.5f, gold.y * 1.5f, gold.z * 1.5f, 1.f};
            rw::vec4 const base{body_color.x, body_color.y, body_color.z, 1.f};
            for (int i = 0; i < rows; ++i) {
                float const f = mathf::inverse_lerp(0.f, float(rows - 1), float(i));
                float const along = mathf::lerp(0.05f, 0.9f, f);
                auto const sd = spine_position(g, along, alpha);
                for (int j = 0; j < lines; ++j) {
                    float const sp = side_pos(i, j);
                    float const scale = 0.5f * mathf::lerp(0.35f, 1.f, 1.f - along);
                    // fully decorated (weaverTier 4): the bigger feathers are gone
                    if (scale > 0.35f)
                        continue;
                    vec2 const at = sd.pos + sd.perp * ((sd.rad + 0.5f) * sp);
                    float const rot = aim(vec2{0.f, 0.f}, sd.dir) - mathf::lerp(0.f, 30.f, 1.f - along) * mathf::sign(sp);
                    vec2 const up = custom::deg_to_vec(rot);
                    vec2 const across = custom::perpendicular_vector(up);
                    // WeaverFeather: a leaf 40px long from its anchor, gold at the tip
                    float const len = 40.f * scale;
                    float const half = 7.f * mathf::lerp(scale, 1.f, 0.25f) * 0.8f;
                    vec2 const b0 = at - up * (len * 0.1f);
                    vec2 const t0 = b0 + up * len;
                    vec2 const m0 = b0 + up * (len * 0.45f) + across * (half * (sp > 0.f ? 1.f : -1.f));
                    vec2 const m1 = b0 + up * (len * 0.45f) - across * (half * 0.4f * (sp > 0.f ? 1.f : -1.f));
                    for (auto const& tri : {std::array<vec2, 3>{b0, m0, t0}, std::array<vec2, 3>{b0, t0, m1}}) {
                        draw_cmd c;
                        c.is_triangle = true;
                        c.vertex_colors = true;
                        for (int k = 0; k < 3; ++k) {
                            c.tri[k] = tri[size_t(k)];
                            float const t = mathf::clamp01(rw::dot(tri[size_t(k)] - b0, up) / mathf::max(len, 0.01f));
                            c.vcol[k] = base + (tip - base) * t;
                        }
                        out.push_back(c);
                    }
                }
            }
            // the halo: three rings (10, 25, 50px) around the head, the middle one glowing
            float const t = (float(g.ticks) + alpha) / 40.f;
            float const n11 = std::sin(t * 0.2f * 6.2831f) * 0.25f;
            float n12 = std::sin(t * 0.1f * 6.2831f + 1.7f) * 0.8f;
            n12 = mathf::pow(std::fabs(n12), 0.75f) * mathf::sign(n12);
            float const strength = 0.55f * g.halo_flux;
            vec2 const hp = lerp_part(g.head);
            rw::vec3 const sat{1.f, 0.73f, 0.368f};
            constexpr int divs = 64;
            for (int k = 0; k < divs; ++k) {
                auto const ring = [&](int kk, float rad) {
                    float const a = float(kk) / float(divs) * 6.2831855f;
                    return hp + vec2{std::cos(-a), std::sin(-a)} * rad;
                };
                // WeaverGlow's banding: eight soft lobes around the ring
                auto const band = [&](int kk) {
                    float const u = float(kk % divs) / float(divs) * 8.f;
                    float const pp = u - 2.f * std::floor(u / 2.f);
                    float const ping = pp > 1.f ? 2.f - pp : pp;
                    return mathf::clamp01(0.55f + mathf::pow(ping, 2.f + n12) * (0.45f + n11 * 0.2f));
                };
                for (int layer = 0; layer < 2; ++layer) {
                    float const r0 = layer == 0 ? 10.f : 25.f, r1 = layer == 0 ? 25.f : 50.f;
                    float const a0 = layer == 0 ? 0.f : 1.f, a1 = layer == 0 ? 1.f : 0.f;
                    vec2 const p00 = ring(k, r0), p01 = ring(k, r1), p10 = ring(k + 1, r0), p11 = ring(k + 1, r1);
                    float const s0 = band(k) * strength, s1 = band(k + 1) * strength;
                    rw::vec4 const c00{sat.x, sat.y, sat.z, a0 * s0}, c01{sat.x, sat.y, sat.z, a1 * s0};
                    rw::vec4 const c10{sat.x, sat.y, sat.z, a0 * s1}, c11{sat.x, sat.y, sat.z, a1 * s1};
                    draw_cmd c;
                    c.is_triangle = true;
                    c.vertex_colors = true;
                    c.tri[0] = p00; c.tri[1] = p01; c.tri[2] = p10;
                    c.vcol[0] = c00; c.vcol[1] = c01; c.vcol[2] = c10;
                    out.push_back(c);
                    c.tri[0] = p10; c.tri[1] = p01; c.tri[2] = p11;
                    c.vcol[0] = c10; c.vcol[1] = c01; c.vcol[2] = c11;
                    out.push_back(c);
                }
            }
        }
    }
}
