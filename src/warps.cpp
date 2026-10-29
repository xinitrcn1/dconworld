#include <algorithm>
#include <cmath>
#include <vector>

#include "warps.hpp"
#include "centipede.hpp"
#include "cicada.hpp"
#include "dropbug.hpp"
#include "lizard.hpp"
#include "items.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "player_graphics.hpp"

namespace warps {
    using rw::vec2;
    using world::room_view;
    using world::warp_state;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // WarpPoint.successfullWarpCooldownFrames
        constexpr float cooldown_frames = 1200.f;
        constexpr float ripple_cooldown_frames = 10.f;

        float trigger_activation_time(world::warp_effect const& e) { return 50.f + float(e.trigger_duration); }
        float activate_animation_time(world::warp_effect const& e) { return 50.f + float(e.active_duration); }

        dcon::warp_placed_id placed_of(sys::game_state const& state, dcon::warp_point_id w) {
            return state.world.warp_point_get_placed(w);
        }
        dcon::room_id room_of(sys::game_state const& state, dcon::warp_point_id w) {
            return state.world.warp_point_get_room_from_warp_point_location(w);
        }
        vec2 pos_of(sys::game_state const& state, dcon::warp_point_id w) {
            return state.world.warp_placed_get_pos(placed_of(state, w));
        }

        bool player_usable(sys::game_state const& state, dcon::player_id p, dcon::room_id room) {
            auto const& w = state.world;
            return w.physical_object_get_room_from_object_location(player::body(state, p)) == room && !w.player_get_in_shortcut(p)
                && !w.player_get_dead(p) && !w.player_get_eaten(p);
        }
        vec2 main_chunk(sys::game_state const& state, dcon::player_id p) {
            return state.world.body_chunk_get_pos(player::body_chunk(state, p, 0));
        }

        // where a warp leads: its room and position, or nothing (a dynamic
        // warp with nowhere to go)
        struct destination {
            dcon::room_id room;
            vec2 pos{0.f, 0.f};
        };

        dcon::region_id region_of(sys::game_state const& state, dcon::room_id room) {
            return state.world.room_get_region_from_region_membership(room);
        }

        // WarpPoint.ChooseDynamicWarpTarget, simplified: any good
        // DynamicWarpTarget of another region the ripple level allows, picked
        // with the warp's seed and the number of dynamic warps taken
        destination dynamic_destination(sys::game_state const& state, dcon::warp_point_id wp) {
            auto const& w = state.world;
            auto const here = region_of(state, room_of(state, wp));
            std::vector<dcon::warp_target_id> options;
            w.for_each_warp_target([&](dcon::warp_target_id t) {
                auto const room = w.warp_target_get_room_from_warp_target_location(t);
                float const req = w.warp_target_get_ripple_requirement(t);
                if (!room || region_of(state, room) == here || w.warp_target_get_bad_warp(t) || req >= 999.f)
                    return;
                if (req > 1.f && req > state.ripple_level)
                    return;
                options.push_back(t);
            });
            if (options.empty())
                return {};
            uint32_t h = w.warp_placed_get_seed(placed_of(state, wp)) ^ (state.dynamic_warps_taken * 0x9E3779B1u);
            h ^= h >> 15;
            h *= 0x2C1B3C6Du;
            h ^= h >> 12;
            auto const t = options[h % options.size()];
            return destination{w.warp_target_get_room_from_warp_target_location(t), w.warp_target_get_pos(t)};
        }

        destination destination_of(sys::game_state const& state, dcon::warp_point_id wp) {
            auto const& w = state.world;
            auto const placed = placed_of(state, wp);
            auto const name = w.warp_placed_get_dest_room(placed);
            if (!name)
                return dynamic_destination(state, wp);
            auto const room = state.find_room(state.to_string_view(name));
            if (!room)
                return {};
            if (auto const p = w.warp_placed_get_dest_pos(placed); p.has_value())
                return destination{room, *p};
            // no position: the warp of that room leading back here, else any warp, else the middle
            auto const src_name = w.room_get_name(room_of(state, wp));
            destination d{room, vec2{float(w.room_get_width(room)) * 10.f, float(w.room_get_height(room)) * 10.f}};
            bool exact = false;
            w.room_for_each_warp_placed_location(room, [&](dcon::warp_placed_location_id l) {
                auto const o = w.warp_placed_location_get_placed(l);
                bool const back = w.warp_placed_get_dest_room(o)
                    && state.to_string_view(w.warp_placed_get_dest_room(o)) == state.to_string_view(src_name);
                if (!exact && (back || d.pos == vec2{float(w.room_get_width(room)) * 10.f, float(w.room_get_height(room)) * 10.f})) {
                    d.pos = w.warp_placed_get_pos(o);
                    exact = back;
                }
            });
            return d;
        }

        // WarpPoint.transportable
        bool transportable(sys::game_state const& state, dcon::warp_point_id wp) {
            auto const& w = state.world;
            auto const placed = placed_of(state, wp);
            return !one_way_exit(state, placed) && (active_ripple_layer(state) == 1) == bool(w.warp_placed_get_ripple_warp(placed))
                && (!w.warp_placed_get_limited_use(placed) || w.warp_point_get_uses(wp) > 0) && w.warp_point_get_cooldown_timer(wp) <= 0.f
                && w.warp_point_get_state(wp) != warp_state::closed;
        }

        void change_state(sys::game_state& state, dcon::warp_point_id wp, warp_state next);

        // WarpPoint.SuckInCreatures: zero gravity around the warp, the
        // Watcher drawn into its middle, everything else spiralling in
        void suck_in(sys::game_state& state, dcon::warp_point_id wp) {
            auto& w = state.world;
            auto& seq = state.warp;
            auto const placed = placed_of(state, wp);
            float const act = activate_animation_time(w.warp_placed_get_effect(placed));
            float const at = float(w.warp_point_get_activation_time(wp));
            float const num = mathf::min(1.f, at / (act / 2.f));
            float const t = mathf::min(1.f, at / act);
            vec2 const pos = pos_of(state, wp);
            vec2 const centre{pos.x, pos.y + 30.f};
            auto const room = room_of(state, wp);
            auto const hold = [&](dcon::physical_object_id o) {
                if (std::find(seq.pulled.begin(), seq.pulled.end(), o) != seq.pulled.end())
                    return;
                seq.pulled.push_back(o);
                seq.pulled_gravity.push_back(w.physical_object_get_g(o));
                w.physical_object_set_g(o, 0.f);
            };
            // the Watcher(s) close enough float into the middle
            std::vector<dcon::physical_object_id> players_held;
            w.for_each_player([&](dcon::player_id p) {
                if (!player_usable(state, p, room) || !custom::dist_less(pos, main_chunk(state, p), suck_radius))
                    return;
                centipede::release_player(state, p);
                dropbug::release_player(state, p);
                cicada::release_player(state, p);
                lizard::release_player(state, p);
                w.player_set_warp_point_cooldown(p, 80);
                if (rw::distance(pos, main_chunk(state, p)) >= pull_radius && p != w.warp_point_get_triggered_by(wp))
                    return;
                auto const o = player::body(state, p);
                hold(o);
                players_held.push_back(o);
                w.player_set_warp_held(p, 1);
                auto const c0 = player::body_chunk(state, p, 0);
                auto const c1 = player::body_chunk(state, p, 1);
                w.body_chunk_get_vel(c0) *= mathf::lerp(1.f, 0.95f, num);
                w.body_chunk_get_vel(c1) *= mathf::lerp(1.f, 0.95f, num);
                vec2 const p0 = w.body_chunk_get_pos(c0);
                float const d = rw::distance(p0, centre);
                w.body_chunk_get_vel(c0) += custom::dir_vec(p0, centre)
                    * (custom::lerp_map(d, 20.f, pull_radius * 0.75f, 2.f * num, 5.f * num) * mathf::inverse_lerp(0.f, 20.f, d));
                w.player_set_warp_protection(p, 10);
            });
            // everything else within reach swirls in (first chunk pulled, the
            // rest follows through the object's own connections)
            auto const swirl = [&](dcon::physical_object_id o) {
                if (std::find(players_held.begin(), players_held.end(), o) != players_held.end())
                    return;
                auto const c = physics::chunk(state, o, 0);
                vec2 const cp = w.body_chunk_get_pos(c);
                float const num2 = rw::distance(pos, cp);
                if (num2 >= pull_radius * 2.5f || num2 > suck_radius)
                    return;
                hold(o);
                vec2 const a = custom::deg_to_vec(float((int32_t(std::atan2(cp.x - pos.x, cp.y - pos.y) / 6.2831855f * 360.f) + 60) % 360));
                vec2 goal = pos + a * (pull_radius * mathf::lerp(0.6f, 0.2f, t));
                float far_pull = 4.f * num;
                float near_pull = 24.f * num;
                if (num2 > pull_radius * 1.5f) {
                    goal = pos;
                    far_pull = 16.f * num;
                    near_pull = 3.f * num;
                }
                float const mass = mathf::min(0.75f, w.body_chunk_get_mass(c));
                float const d = rw::distance(cp, goal);
                w.body_chunk_get_vel(c) *= mathf::lerp(1.f, 0.95f, num);
                w.body_chunk_get_vel(c) += custom::dir_vec(cp, goal)
                    * (custom::lerp_map(d, 30.f, pull_radius * 1.5f, near_pull * mass, far_pull * mass) * mathf::inverse_lerp(0.f, 30.f, d));
            };
            w.for_each_item([&](dcon::item_id it) {
                auto const o = items::body(state, it);
                // spears stuck in walls stay
                if (w.physical_object_get_room_from_object_location(o) != room || w.item_get_mode(it) == world::weapon_mode::stuck_in_wall
                || w.item_get_mode(it) == world::weapon_mode::carried || w.item_get_mode(it) == world::weapon_mode::on_back)
                    return;
                swirl(o);
            });
            w.for_each_centipede([&](dcon::centipede_id c) {
                auto const o = centipede::body(state, c);
                if (w.physical_object_get_room_from_object_location(o) != room)
                    return;
                w.centipede_set_stun(c, std::max(w.centipede_get_stun(c), 20));
                swirl(o);
            });
            w.for_each_dropbug([&](dcon::dropbug_id d) {
                auto const o = dropbug::body(state, d);
                if (w.physical_object_get_room_from_object_location(o) != room)
                    return;
                w.dropbug_set_stun(d, std::max(w.dropbug_get_stun(d), 20));
                swirl(o);
            });
            w.for_each_cicada([&](dcon::cicada_id c) {
                auto const o = cicada::body(state, c);
                if (w.physical_object_get_room_from_object_location(o) != room)
                    return;
                w.cicada_set_stun(c, std::max(w.cicada_get_stun(c), 20));
                swirl(o);
            });
            w.for_each_lizard([&](dcon::lizard_id l) {
                auto const o = lizard::body(state, l);
                if (w.physical_object_get_room_from_object_location(o) != room)
                    return;
                w.lizard_set_stun(l, std::max(w.lizard_get_stun(l), 20));
                swirl(o);
            });
            w.for_each_player([&](dcon::player_id p) {
                if (player_usable(state, p, room))
                    swirl(player::body(state, p));
            });
        }

        // puts an object down at `at`, still
        void place_object(sys::game_state& state, dcon::physical_object_id o, dcon::room_id room, vec2 at) {
            auto& w = state.world;
            w.physical_object_set_room_from_object_location(o, room);
            vec2 const first = w.body_chunk_get_pos(physics::chunk(state, o, 0));
            w.physical_object_for_each_chunk_ownership_as_owner(o, [&](dcon::chunk_ownership_id co) {
                auto const c = w.chunk_ownership_get_chunk(co);
                physics::hard_set_position(state, c, at + (w.body_chunk_get_pos(c) - first));
                w.body_chunk_set_vel(c, vec2{0.f, 0.f});
                w.body_chunk_set_contact_point(c, rw::ivec2{0, 0});
            });
        }

        // WarpPoint.PerformWarp + WarpTransfer: the Watcher and everything the
        // pull held comes out at the destination, at once (vanilla lets the
        // other objects out one by one)
        void perform_warp(sys::game_state& state, dcon::warp_point_id wp) {
            auto& w = state.world;
            auto& seq = state.warp;
            auto const placed = placed_of(state, wp);
            auto const dest = destination_of(state, wp);
            if (!w.warp_placed_get_dest_room(placed))
                ++state.dynamic_warps_taken;
            if (w.warp_placed_get_limited_use(placed) && w.warp_point_get_uses(wp) > 0)
                w.warp_point_set_uses(wp, int16_t(w.warp_point_get_uses(wp) - 1));
            // warping wakes the Watcher's ripple sense
            if (state.ripple_level < 1.f)
                state.ripple_level = 1.f;
            if (!dest.room) {
                change_state(state, wp, warp_state::cool_down);
                return;
            }
            spawn_room(state, dest.room);
            // the warp of the destination it comes out of (the matching exit)
            dcon::warp_point_id exit;
            float best = 300.f;
            w.room_for_each_warp_point_location(dest.room, [&](dcon::warp_point_location_id l) {
                auto const o = w.warp_point_location_get_point(l);
                float const d = rw::distance(pos_of(state, o), dest.pos);
                if (d < best) {
                    best = d;
                    exit = o;
                }
            });
            vec2 const out = exit ? pos_of(state, exit) : dest.pos;
            seq.to_room = dest.room;
            seq.to_pos = out;
            auto const src = room_of(state, wp);
            auto const objects = seq.pulled;
            for (size_t i = 0; i < objects.size(); ++i) {
                auto const o = objects[i];
                if (!w.physical_object_is_valid(o) || w.physical_object_get_room_from_object_location(o) != src)
                    continue;
                // in a loose ring around the exit, the Watcher in the middle
                float const ang = float(i) * 2.3999632f;
                bool const is_player = bool(w.physical_object_get_player_from_player_body(o));
                vec2 const at = is_player ? out : out + vec2{std::sin(ang), std::cos(ang)} * (20.f + 6.f * float(i));
                place_object(state, o, dest.room, at);
            }
            // whatever the travellers hold comes along
            w.for_each_player([&](dcon::player_id p) {
                if (w.physical_object_get_room_from_object_location(player::body(state, p)) != dest.room)
                    return;
                w.for_each_item([&](dcon::item_id it) {
                    if (items::grabber(state, it) == p || w.player_get_back_spear(p) == it)
                        w.physical_object_set_room_from_object_location(items::body(state, it), dest.room);
                });
                items::carry_update(state, p);
                player_graphics::reset(state, p);
            });
            seq.transferred = true;
            if (exit)
                change_state(state, exit, warp_state::exit);
            change_state(state, wp, w.warp_placed_get_limited_use(placed) && w.warp_point_get_uses(wp) <= 0 ? warp_state::closed : warp_state::cool_down);
        }

        // gives the pulled objects their gravity back
        void release_pulled(sys::game_state& state) {
            auto& w = state.world;
            auto& seq = state.warp;
            for (size_t i = 0; i < seq.pulled.size(); ++i)
                if (w.physical_object_is_valid(seq.pulled[i]))
                    w.physical_object_set_g(seq.pulled[i], seq.pulled_gravity[i]);
            seq.pulled.clear();
            seq.pulled_gravity.clear();
            w.for_each_player([&](dcon::player_id p) { w.player_set_warp_held(p, 0); });
        }

        // WarpPoint.ChangeState
        void change_state(sys::game_state& state, dcon::warp_point_id wp, warp_state next) {
            auto& w = state.world;
            auto const placed = placed_of(state, wp);
            auto const cur = w.warp_point_get_state(wp);
            if (cur == warp_state::ready)
                w.warp_point_set_trigger_time(wp, 0.f);
            else if (cur == warp_state::enter)
                w.warp_point_set_activation_time(wp, 0);
            else if (cur == warp_state::exit)
                w.for_each_player([&](dcon::player_id p) { w.player_set_levitation_active(p, 0); });
            else if (cur == warp_state::cool_down)
                w.warp_point_set_cooldown_timer(wp, 0.f);
            if (next == warp_state::enter) {
                auto const room = room_of(state, wp);
                w.for_each_player([&](dcon::player_id p) {
                    if (player_usable(state, p, room))
                        w.player_set_warp_point_cooldown(p, 80);
                });
                w.warp_point_set_activation_time(wp, 0);
                auto& seq = state.warp;
                if (!seq.active) {
                    seq = sys::game_state::warp_sequence{};
                    seq.active = true;
                    seq.origin = wp;
                    seq.from_room = room;
                    seq.from_pos = pos_of(state, wp);
                    seq.to_room = room;
                    seq.to_pos = seq.from_pos;
                    seq.effect = w.warp_placed_get_effect(placed);
                    seq.ripple = w.warp_placed_get_ripple_warp(placed);
                    seq.duration = activate_animation_time(seq.effect) * 2.f;
                }
            } else if (next == warp_state::cool_down) {
                w.warp_point_set_cooldown_timer(wp, w.warp_placed_get_ripple_warp(placed) ? ripple_cooldown_frames : cooldown_frames);
                w.warp_point_set_closes_early(wp, true);
            } else if (next == warp_state::exit) {
                w.warp_point_set_open_triggered(wp, true);
                w.warp_point_set_open(wp, 1.f);
            }
            w.warp_point_set_state(wp, next);
        }

        // WarpPoint.UpdateWarpTear: the tear opens while the Watcher is near
        void update_tear(sys::game_state& state, dcon::warp_point_id wp) {
            auto& w = state.world;
            auto const placed = placed_of(state, wp);
            w.warp_point_set_last_open(wp, w.warp_point_get_open(wp));
            w.warp_point_set_last_fade(wp, w.warp_point_get_fade(wp));
            bool const in_sequence = state.warp.active;
            float const num = in_sequence ? 80.f : 240.f;
            float fade = w.warp_point_get_fade(wp);
            bool const closed = w.warp_point_get_state(wp) == warp_state::closed;
            // one way exits only show while something comes out of them
            if (closed || (one_way_exit(state, placed) && w.warp_point_get_state(wp) != warp_state::exit))
                fade = mathf::max(0.f, fade - 1.f / 80.f);
            else
                fade = mathf::min(1.f, fade + 1.f / num);
            w.warp_point_set_fade(wp, fade);
            if (!transportable(state, wp) && w.warp_placed_get_ripple_warp(placed) && w.warp_point_get_state(wp) != warp_state::exit)
                w.warp_point_set_open_triggered(wp, false);
            float open = w.warp_point_get_open(wp);
            if (w.warp_point_get_open_triggered(wp))
                open = mathf::min(1.f, open + 1.f / num);
            else
                open = mathf::max(0.f, open - 1.f / num);
            w.warp_point_set_open(wp, open);
        }

        void update_point(sys::game_state& state, room_view const& room, dcon::warp_point_id wp) {
            auto& w = state.world;
            auto const placed = placed_of(state, wp);
            vec2 const pos = pos_of(state, wp);
            update_tear(state, wp);
            // a warp of the other layer gives itself away with faint rings
            if (other_layer(state, wp) && !one_way_exit(state, placed)) {
                float t = w.warp_point_get_shockwave_timer(wp) + 1.f / 60.f;
                float const limit = w.warp_point_get_shockwave_limit(wp);
                auto& rng = state.random;
                if (t >= limit && !w.warp_point_get_second_shockwave(wp)) {
                    w.warp_point_set_second_shockwave(wp, true);
                    spawn_ripple_ring(state, room.id, pos, 80.f + 180.f * rng.value(), 1.f, 1.f);
                } else if (t >= limit + 0.04f && w.warp_point_get_second_shockwave(wp)) {
                    w.warp_point_set_shockwave_limit(wp, 2.f + 4.f * rng.value());
                    t = 0.f;
                    w.warp_point_set_second_shockwave(wp, false);
                    spawn_ripple_ring(state, room.id, pos, 80.f + 180.f * rng.value(), 1.f, 1.f);
                }
                w.warp_point_set_shockwave_timer(wp, t);
            }
            // WarpPoint.ProvideAir: breathing inside an open tear
            if (room.water) {
                float const open = openness(state, wp, 1.f);
                w.for_each_player([&](dcon::player_id p) {
                    if (open > 0.f && player_usable(state, p, room.id) && custom::dist_less(pos, main_chunk(state, p), pull_radius) && w.player_get_air_in_lungs(p) < 0.9f)
                        w.player_set_air_in_lungs(p, w.player_get_air_in_lungs(p) + open * 0.005f);
                });
            }

            switch (w.warp_point_get_state(wp)) {
            case warp_state::ready: {
                if (!transportable(state, wp) || state.warp.active)
                    break;
                // the closest Watcher that may warp
                dcon::player_id who;
                float best = 0.f;
                w.for_each_player([&](dcon::player_id p) {
                    if (!player_usable(state, p, room.id) || !is_watcher(state, p) || w.player_get_warp_point_cooldown(p) > 0)
                        return;
                    float const d = rw::distance(pos, main_chunk(state, p));
                    if (!who || d < best) {
                        who = p;
                        best = d;
                    }
                    if (d < pull_radius)
                        w.player_set_warp_protection(p, 10);
                });
                bool const early = w.warp_point_get_closes_early(wp);
                float trigger = w.warp_point_get_trigger_time(wp);
                float const needed = trigger_activation_time(w.warp_placed_get_effect(placed));
                if (who) {
                    if (early && best > pull_radius * 6.f)
                        w.warp_point_set_closes_early(wp, false);
                    if (best >= pull_radius * (early ? 2.f : 4.5f))
                        w.warp_point_set_open_triggered(wp, false);
                    if (best < pull_radius * (early ? 1.5f : 3.f))
                        w.warp_point_set_open_triggered(wp, true);
                    if (best < pull_radius && openness(state, wp, 1.f) > 0.5f) {
                        // standing still in it speeds it up, moving about holds it back
                        float num3 = 1.f + mathf::pow(mathf::inverse_lerp(pull_radius, 10.f, best), 0.5f) * 3.f;
                        int32_t const still = w.player_get_touched_no_input_counter(who);
                        if (still > 0) {
                            if (still < 20)
                                num3 *= custom::lerp_map(float(still), 1.f, 20.f, 0.5f, 1.f);
                            else
                                num3 += custom::lerp_map(float(still), 20.f, 120.f, 0.f, 3.f);
                        } else if (trigger < needed / 2.f) {
                            num3 *= 0.5f;
                        } else {
                            num3 = custom::lerp_map(trigger, needed / 2.f, needed, -0.5f, -4.f);
                        }
                        trigger += num3;
                    }
                }
                if (!who || best >= pull_radius) {
                    if (!who)
                        w.warp_point_set_open_triggered(wp, false);
                    trigger = trigger > 0.f ? trigger - 2.f : 0.f;
                }
                w.warp_point_set_trigger_time(wp, mathf::max(0.f, trigger));
                if (who && trigger >= needed) {
                    w.warp_point_set_triggered_by(wp, who);
                    change_state(state, wp, warp_state::enter);
                }
                break;
            }
            case warp_state::enter: {
                suck_in(state, wp);
                float const act = activate_animation_time(w.warp_placed_get_effect(placed));
                if (float(w.warp_point_get_activation_time(wp)) < act)
                    w.warp_point_set_activation_time(wp, w.warp_point_get_activation_time(wp) + 1);
                else
                    perform_warp(state, wp);
                break;
            }
            case warp_state::exit:
                // the Watcher hovers out of the tear while the transition clears
                w.for_each_player([&](dcon::player_id p) {
                    if (!player_usable(state, p, room.id))
                        return;
                    w.player_set_levitation_active(p, 1);
                    w.player_set_levitation_needs_cooldown(p, 1);
                    w.player_set_warp_point_cooldown(p, 80);
                    auto const o = player::body(state, p);
                    w.physical_object_set_g(o, 0.f);
                    for (uint32_t i = 0; i < 2; ++i) {
                        auto const c = player::body_chunk(state, p, i);
                        w.body_chunk_get_vel(c).y = 0.f;
                        if (i == 1)
                            w.body_chunk_get_vel(c).x = 0.f;
                    }
                });
                if (!state.warp.active || state.warp.progress >= state.warp.duration)
                    change_state(state, wp, warp_state::spawn_items);
                break;
            case warp_state::spawn_items:
                // everything already came out with the Watcher
                change_state(state, wp, warp_state::cool_down);
                break;
            case warp_state::cool_down: {
                float closest = -1.f;
                w.for_each_player([&](dcon::player_id p) {
                    if (!player_usable(state, p, room.id))
                        return;
                    float const d = rw::distance(pos, main_chunk(state, p));
                    if (closest < 0.f || d < closest)
                        closest = d;
                });
                float const rate = closest < 0.f ? 5.f : custom::lerp_map(closest, 0.23f * 2.f, 1000.f, 1.f, 5.f);
                w.warp_point_set_open_triggered(wp, false);
                float const t = w.warp_point_get_cooldown_timer(wp) - rate;
                w.warp_point_set_cooldown_timer(wp, t);
                if (t <= 0.f)
                    change_state(state, wp, warp_state::ready);
                break;
            }
            case warp_state::closed:
                w.warp_point_set_open_triggered(wp, false);
                break;
            }
        }
    }

    bool is_watcher(sys::game_state const& state, dcon::player_id p) noexcept {
        return state.world.player_get_stats(p).name == player::slugcat_name::watcher;
    }

    int32_t active_ripple_layer(sys::game_state const& state) noexcept {
        auto const& w = state.world;
        dcon::player_id first;
        w.for_each_player([&](dcon::player_id p) {
            if (!first)
                first = p;
        });
        return first && is_watcher(state, first) && w.player_get_is_camo(first) && state.ripple_level >= 5.f ? 1 : 0;
    }

    bool one_way_exit(sys::game_state const& state, dcon::warp_placed_id w) noexcept {
        return state.world.warp_placed_get_one_way(w) && !state.world.warp_placed_get_one_way_entrance(w);
    }

    bool other_layer(sys::game_state const& state, dcon::warp_point_id wp) noexcept {
        auto const placed = placed_of(state, wp);
        auto const& w = state.world;
        return (active_ripple_layer(state) == 1) != bool(w.warp_placed_get_ripple_warp(placed))
            && (!w.warp_placed_get_limited_use(placed) || w.warp_point_get_uses(wp) > 0);
    }

    float openness(sys::game_state const& state, dcon::warp_point_id wp, float alpha) noexcept {
        auto const& w = state.world;
        return mathf::lerp(w.warp_point_get_last_open(wp), w.warp_point_get_open(wp), alpha)
            * mathf::lerp(w.warp_point_get_last_fade(wp), w.warp_point_get_fade(wp), alpha);
    }

    void spawn_room(sys::game_state& state, dcon::room_id room) {
        auto& w = state.world;
        if (w.room_get_warps_spawned(room))
            return;
        w.room_set_warps_spawned(room, true);
        w.room_for_each_warp_placed_location(room, [&](dcon::warp_placed_location_id l) {
            auto const placed = w.warp_placed_location_get_placed(l);
            auto const wp = w.create_warp_point();
            w.try_create_warp_point_location(wp, room);
            w.warp_point_set_placed(wp, placed);
            w.warp_point_set_uses(wp, w.warp_placed_get_uses(placed));
            w.warp_point_set_fade(wp, 1.f);
            w.warp_point_set_last_fade(wp, 1.f);
            w.warp_point_set_shockwave_limit(wp, 2.f);
            bool const used_up = w.warp_placed_get_limited_use(placed) && w.warp_placed_get_uses(placed) <= 0;
            w.warp_point_set_state(wp, used_up ? warp_state::closed : warp_state::ready);
            if (used_up || one_way_exit(state, placed)) {
                w.warp_point_set_fade(wp, 0.f);
                w.warp_point_set_last_fade(wp, 0.f);
            }
        });
    }

    void update_room(sys::game_state& state, room_view const& room) {
        auto& w = state.world;
        w.room_for_each_warp_point_location(room.id, [&](dcon::warp_point_location_id l) {
            update_point(state, room, w.warp_point_location_get_point(l));
        });
    }

    void update_sequence(sys::game_state& state) {
        auto& seq = state.warp;
        if (!seq.active)
            return;
        seq.last_progress = seq.progress;
        seq.progress += 1.f;
        // the pull lets go once everything is out on the other side
        if (seq.transferred && !seq.pulled.empty())
            release_pulled(state);
        if (seq.progress > seq.duration) {
            release_pulled(state);
            seq.active = false;
        }
    }

    void spawn_ripple_ring(sys::game_state& state, dcon::room_id room, vec2 pos, float rad, float intensity, float seconds) {
        if (state.ripple_rings.size() < 256)
            state.ripple_rings.push_back({room, pos, rad, 0.f, 1.f / (seconds * 40.f), intensity});
    }

    void update_ripple_rings(sys::game_state& state) {
        auto& rings = state.ripple_rings;
        for (auto& r : rings)
            r.life += r.speed;
        rings.erase(std::remove_if(rings.begin(), rings.end(), [](auto const& r) { return r.life >= 1.f; }), rings.end());
    }
}
