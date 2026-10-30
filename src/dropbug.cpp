#include <cmath>
#include <limits>
#include <optional>
#include <vector>

#include "dropbug.hpp"
#include "dens.hpp"
#include "rooms.hpp"
#include "dropbug_graphics.hpp"
#include "items.hpp"
#include "particles.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "player.hpp"

namespace dropbug {
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // CreatureTemplate: DropBug
        constexpr float base_damage_resistance = 0.95f;
        constexpr float base_stun_resistance = 0.6f;
        constexpr float visual_radius = 800.f;
        // CreatureTemplate.bodySize of a Slugcat
        constexpr float slugcat_body_size = 1.f;
        // Drop_Bug_Voice's SpiderHiss samples last about a second: how long
        // voiceSound counts as playing
        constexpr int32_t voice_ticks = 40;
        constexpr float float_max = std::numeric_limits<float>::max();

        using creature::player_of;
        using creature::room_of;
        using creature::total_mass;

        vec2& pos(sys::game_state& state, dcon::creature_id d, uint32_t i) {
            return state.world.body_chunk_get_pos(chunk(state, d, i));
        }
        vec2 pos(sys::game_state const& state, dcon::creature_id d, uint32_t i) {
            return state.world.body_chunk_get_pos(chunk(state, d, i));
        }
        vec2& vel(sys::game_state& state, dcon::creature_id d, uint32_t i) {
            return state.world.body_chunk_get_vel(chunk(state, d, i));
        }

        // abstractCreature.pos: the head's tile
        ivec2 here(sys::game_state const& state, dcon::creature_id d) {
            return room_view::get_tile_position(pos(state, d, 0));
        }

        float object_mass(sys::game_state const& state, dcon::physical_object_id o) {
            float m = 0.f;
            state.world.physical_object_for_each_chunk_ownership_as_owner(o, [&](dcon::chunk_ownership_id co) {
                m += state.world.body_chunk_get_mass(state.world.chunk_ownership_get_chunk(co));
            });
            return m;
        }

        bool accessible(room_view const& room, ivec2 t) {
            return room.inside(t.x, t.y) && pathing::accessible(room, t, pathing::drop_bug);
        }
        bool accessible(room_view const& room, vec2 p) {
            return accessible(room, room_view::get_tile_position(p));
        }

        bool solid(room_view const& room, ivec2 t) {
            return room.get_tile(t).solid();
        }

        ivec2 random_tile(rw::rng& rng, room_view const& room) {
            return ivec2{rng.range(0, room.width), rng.range(0, room.height)};
        }

        float float_dist(ivec2 a, ivec2 b) {
            return rw::magnitude(rw::to_vec2(a - b));
        }

        //
        // The AImap fields DropBugAI looks at (AImapper), from the tiles
        //

        using pathing::narrow_space;
        using pathing::floor_altitude;

        // AItile.smoothedFloorAltitude (AImapper.FindFallRiskOfCurrentTile)
        int32_t smoothed_floor_altitude(room_view const& room, ivec2 t) {
            if (solid(room, t))
                return 100000;
            int32_t alt = floor_altitude(room, t);
            int32_t const reach = custom::int_clamp(alt, 0, 5);
            for (int32_t i = -1; i < 2; i += 2)
                for (int32_t k = 1; k < reach && t.x + i * k >= 0 && t.x + i * k < room.width && !solid(room, ivec2{t.x + i * k, t.y}); ++k)
                    alt = std::min(alt, floor_altitude(room, ivec2{t.x + i * k, t.y}) + k / 2);
            return alt;
        }

        // AItile.fallRiskTile: the lowest tile something falling from `t` (or
        // from the columns beside it) lands on, (-1, -1) over a pit
        ivec2 fall_risk_tile(room_view const& room, ivec2 t) {
            if (solid(room, t))
                return t;
            // the landing tile of column x from height y, if the column is open down to it
            auto const landing = [&](int32_t x, int32_t y) -> std::optional<ivec2> {
                if (x < 0 || x >= room.width || solid(room, ivec2{x, y}))
                    return std::nullopt;
                for (; y > 0 && !solid(room, ivec2{x, y - 1}); --y) {
                }
                if (y == 0 && !solid(room, ivec2{x, -1}))
                    return ivec2{-1, -1};
                return ivec2{x, y};
            };
            ivec2 best = *landing(t.x, t.y);
            for (int32_t i = -1; i < 2; i += 2)
                if (auto const l = landing(t.x + i, t.y); l && l->y < best.y)
                    best = *l;
            return best;
        }

        // Room.CameraViewingPoint
        int32_t camera_viewing_point(sys::game_state const& state, dcon::room_id room, vec2 p) {
            int32_t found = -1, i = 0;
            state.world.room_for_each_camera_location(room, [&](dcon::camera_location_id cl) {
                vec2 const c = state.world.room_camera_get_position(state.world.camera_location_get_camera(cl));
                if (found < 0 && p.x > c.x && p.x < c.x + 1366.f && p.y > c.y && p.y < c.y + 768.f)
                    found = i;
                ++i;
            });
            return found;
        }

        // Room.ceilingTiles: open tiles under a solid one, over open space,
        // above the water
        std::vector<ivec2> const& ceiling_tiles(sys::game_state& state, room_view const& room) {
            auto& rt = state.runtime(room.id);
            uint16_t const version = state.world.room_get_geometry_version(room.id);
            if (rt.ceiling_version == version)
                return rt.ceiling_tiles;
            rt.ceiling_version = version;
            rt.ceiling_tiles.clear();
            for (int32_t x = 0; x < room.width; ++x)
                for (int32_t y = 0; y < room.height - 1; ++y)
                    if (!solid(room, ivec2{x, y}) && solid(room, ivec2{x, y + 1}) && !solid(room, ivec2{x, y - 1}) && y > room.default_water_level)
                        rt.ceiling_tiles.push_back(ivec2{x, y});
            return rt.ceiling_tiles;
        }

        // DropBugAI.ValidCeilingSpot
        bool valid_ceiling_spot(sys::game_state const& state, room_view const& room, ivec2 t) {
            if (room.get_terrain(t.x, t.y) != world::terrain::air || !solid(room, t + ivec2{0, 1}) || !solid(room, t + ivec2{0, 2}) || solid(room, t + ivec2{0, -1}))
                return false;
            if (narrow_space(room, t))
                return false;
            if (smoothed_floor_altitude(room, t) < 6 || floor_altitude(room, t) < 6)
                return false;
            ivec2 const fall = fall_risk_tile(room, t);
            // (without Remix the exit reachability check that follows returns
            // true either way)
            return camera_viewing_point(state, room.id, room_view::middle_of_tile(t)) == camera_viewing_point(state, room.id, room_view::middle_of_tile(fall));
        }

        // AImap.ExitDistanceForCreature for a StandardGroundCreature: tiles
        // to the room exit, -1 unreachable
        int32_t exit_distance(sys::game_state& state, room_view const& room, ivec2 from, ivec2 exit) {
            if (!room.inside(from.x, from.y) || !pathing::accessible(room, from, pathing::standard_ground))
                return -1;
            uint16_t const d = pathing::distance(state, pathing::field_to(state, room.id, exit, pathing::standard_ground), from);
            return d == pathing::unreachable ? -1 : int32_t(d / pathing::step_cost);
        }

        // DropBugAI.CeilingSpotScore: lower is better (the AImap visibility
        // terms aren't ported)
        float ceiling_spot_score(sys::game_state& state, room_view const& room, ivec2 t) {
            if (!valid_ceiling_spot(state, room, t))
                return float_max;
            auto const& w = state.world;
            float num = std::fabs(16.f - float(smoothed_floor_altitude(room, t))) + std::fabs(16.f - float(floor_altitude(room, t)));
            ivec2 const fall = fall_risk_tile(room, t);
            for (int32_t i = -2; i <= 2; ++i) {
                auto const tile = room.get_tile(t + ivec2{i, 0});
                if (tile.solid())
                    num += 10.f;
                if (tile.any_beam())
                    num += 1.f;
                if (pathing::tile_access(room, fall + ivec2{i, 0}) != pathing::access::floor)
                    num += 2.f;
            }
            int32_t most = std::numeric_limits<int32_t>::min(), least = std::numeric_limits<int32_t>::max();
            w.room_for_each_shortcut_location(room.id, [&](dcon::shortcut_location_id l) {
                auto const s = w.shortcut_location_get_shortcut(l);
                if (w.shortcut_get_type(s) != world::shortcut_type::room_exit)
                    return;
                int32_t const dist = exit_distance(state, room, fall, w.shortcut_get_start(s));
                if (dist > 0)
                    num -= 3.f;
                most = std::max(most, dist);
                least = std::min(least, dist);
            });
            if (least < 20)
                num += 500.f;
            num -= float(least) / float(most) * 100.f;
            w.room_for_each_shortcut_location(room.id, [&](dcon::shortcut_location_id l) {
                auto const s = w.shortcut_location_get_shortcut(l);
                if (float_dist(w.shortcut_get_start(s), t) < 5.f)
                    num += w.shortcut_get_type(s) == world::shortcut_type::room_exit ? 500.f : 50.f;
            });
            return num;
        }

        // the first room exit's entrance: where LeaveRoom heads (there is no
        // migration between rooms yet)
        std::optional<ivec2> exit_tile(sys::game_state const& state, room_view const& room) {
            std::optional<ivec2> e;
            state.world.room_for_each_shortcut_location(room.id, [&](dcon::shortcut_location_id l) {
                auto const s = state.world.shortcut_location_get_shortcut(l);
                if (!e && state.world.shortcut_get_type(s) == world::shortcut_type::room_exit)
                    e = state.world.shortcut_get_start(s);
            });
            return e;
        }

        //
        // DropBugAI
        //

        bool is_sitting_in_ceiling(sys::game_state const& state, dcon::creature_id d, room_view const& room) {
            auto const& w = state.world;
            ivec2 const cp = w.dropbug_data_get_ceiling(creature::dropbug_data(w, d)).ceiling_pos;
            return ::dropbug::behavior(w.creature_get_behavior(d)) == behavior::sit_in_ceiling && custom::dist_less(pos(state, d, 1), room_view::middle_of_tile(cp), 40.f)
                && w.creature_get_destination(d) == cp && valid_ceiling_spot(state, room, cp);
        }

        // CeilingSitModule.AnyWhereToSitInRoom
        bool anywhere_to_sit_in_room(sys::game_state& state, dcon::creature_id d, room_view const& room) {
            auto const& c = state.world.dropbug_data_get_ceiling(creature::dropbug_data(state.world, d));
            return c.anywhere_to_sit || c.anywhere_to_sit_counter < int32_t(ceiling_tiles(state, room).size());
        }

        dcon::player_id tracked(sys::game_state const& state, dcon::creature_id d) {
            return player_of(state, state.world.creature_get_seen_player(d));
        }

        // DropBugAI.UpdateDynamicRelationship towards a slugcat: Eats (its
        // default relationship), by body size, and much more for the one it
        // attacked last
        float prey_intensity(sys::game_state const& state, dcon::creature_id d, dcon::player_id p) {
            float intensity = mathf::inverse_lerp(0.1f, 1.5f, slugcat_body_size);
            if (auto const target = player_of(state, state.world.dropbug_data_get_target_player(creature::dropbug_data(state.world, d))))
                intensity = mathf::pow(intensity, target == p ? 0.1f : 3.f);
            return intensity;
        }

        // ArtificialIntelligence.VisualScore + DropBugAI.VisualScore: it sees
        // ahead of its head (the chunks' movement based bonus isn't ported)
        bool visual_contact(sys::game_state const& state, dcon::creature_id d, room_view const& room, vec2 look) {
            vec2 const eye = pos(state, d, 0);
            if (!custom::dist_less(eye, look, visual_radius))
                return false;
            float score = mathf::inverse_lerp(visual_radius, 0.f, rw::distance(eye, look));
            if (narrow_space(room, room_view::get_tile_position(look)))
                score -= 0.5f;
            score -= mathf::pow(mathf::inverse_lerp(0.4f, -0.6f, rw::dot(rw::normalized(pos(state, d, 1) - eye), rw::normalized(eye - look))), 2.f);
            return score > 0.f && room.visual_contact(eye, look);
        }

        // Tracker: the closest slugcat it can see, remembered for a while
        void tracker_update(sys::game_state& state, dcon::creature_id d, room_view const& room) {
            auto& w = state.world;
            dcon::player_id prey;
            float best = float_max;
            rooms::for_each_player(state, room.id, [&](dcon::player_id p) {
                if (w.player_get_in_shortcut(p))
                    return;
                for (uint32_t i = 0; i < 2; ++i) {
                    vec2 const pp = w.body_chunk_get_pos(player::body_chunk(state, p, i));
                    float const dist = rw::distance(pp, pos(state, d, 0));
                    if (dist < best && visual_contact(state, d, room, pp)) {
                        best = dist;
                        prey = p;
                    }
                }
            });
            int32_t& since = w.creature_get_ticks_since_seen(d);
            w.creature_set_sees_player(d, prey ? 1 : 0);
            if (prey) {
                w.creature_set_seen_player(d, uint8_t(prey.index() + 1));
                w.creature_set_last_seen_pos(d, w.body_chunk_get_pos(player::body_chunk(state, prey, 0)));
                since = 0;
            } else if (w.creature_get_seen_player(d) && ++since > 1500) {
                // Tracker(forgetCounter 1500)
                w.creature_set_seen_player(d, 0);
            }
        }

        // PreyTracker.Utility (distance estimated in tiles, the chance of
        // finding it from how long ago it was seen)
        float prey_utility(sys::game_state const& state, dcon::creature_id d) {
            auto const p = tracked(state, d);
            if (!p)
                return 0.f;
            auto const& w = state.world;
            float const dist = mathf::max(0.01f, float_dist(here(state, d), room_view::get_tile_position(w.creature_get_last_seen_pos(d))));
            // PreyTracker(sureToGetPreyDistance 5, sureToLosePreyDistance 40)
            float const num = mathf::lerp(1.f - mathf::inverse_lerp(5.f, 40.f, dist), mathf::lerp(5.f, 40.f, 0.25f) / dist, 0.5f);
            float const chance = custom::lerp_map(float(w.creature_get_ticks_since_seen(d)), 0.f, 1500.f, 1.f, 0.f);
            return mathf::min(num, chance) * mathf::pow(prey_intensity(state, d, p), 0.75f);
        }

        // StuckTracker.Update (trackPastPositions): stuck while most of the
        // older tracked positions are close to where it is
        void stuck_update(sys::game_state& state, dcon::creature_id d) {
            auto& w = state.world;
            auto& s = w.dropbug_data_get_stuck(creature::dropbug_data(w, d));
            ivec2 const at = here(state, d);
            ivec2 const goal = w.creature_get_destination(d);
            int32_t close = 0;
            if (std::abs(at.x - goal.x) + std::abs(at.y - goal.y) > 3) {
                s.head = (s.head + stuck_tracker::tracked - 1) % stuck_tracker::tracked;
                s.past[s.head] = at;
                s.count = std::min(s.count + 1, stuck_tracker::tracked);
                for (int32_t j = 40; j < s.count; ++j)
                    if (float_dist(at, s.past[(s.head + j) % stuck_tracker::tracked]) < 4.f)
                        ++close;
            }
            s.counter = custom::int_clamp(close > 30 ? s.counter + 1 : s.counter - 2, 0, 200);
        }
        float stuck_utility(sys::game_state const& state, dcon::creature_id d) {
            return mathf::inverse_lerp(100.f, 200.f, float(state.world.dropbug_data_get_stuck(creature::dropbug_data(state.world, d)).counter));
        }

        // InjuryTracker(0.4).Utility
        float injury_utility(sys::game_state const& state, dcon::creature_id d) {
            return custom::s_curve(mathf::inverse_lerp(0.2f, 0.9f, 1.f - mathf::clamp01(state.world.creature_get_health(d))), 0.4f);
        }

        // CeilingSitModule.Utility
        float ceiling_utility(sys::game_state& state, dcon::creature_id d, room_view const& room) {
            auto const& w = state.world;
            if (grasped(state, d))
                return 0.f;
            if (!anywhere_to_sit_in_room(state, d, room))
                return 0.f;
            if (is_sitting_in_ceiling(state, d, room))
                return 1.f;
            return mathf::inverse_lerp(20.f, 40.f, float(w.dropbug_data_get_ceiling(creature::dropbug_data(w, d)).win_streak));
        }

        // CeilingSitModule.DynamicCeilingSpotScore
        float dynamic_ceiling_spot_score(sys::game_state& state, dcon::creature_id d, room_view const& room, ivec2 test) {
            float num = ceiling_spot_score(state, room, test);
            if (num == float_max)
                return num;
            auto const& w = state.world;
            auto const& c = w.dropbug_data_get_ceiling(creature::dropbug_data(w, d));
            num -= custom::lerp_map(float_dist(test, here(state, d)), 20.f, 60.f, 100.f, 0.f);
            num += custom::lerp_map(float_dist(test, c.stay_away_from_pos), 0.f, 15.f, 400.f, 0.f);
            num -= custom::lerp_map(float_dist(test, c.random_attractor), float(c.room_size / 4), float(c.room_size / 2), 150.f, 0.f);
            // the creatures it tracks: the slugcat, while alive
            if (auto const p = tracked(state, d); p && !w.player_get_dead(p)) {
                float const intensity = prey_intensity(state, d, p);
                if (intensity > 0.f)
                    num += custom::lerp_map(float_dist(test, room_view::get_tile_position(w.creature_get_last_seen_pos(d))), 3.f + 8.f * intensity, 2.f, 0.f, intensity * 70.f, 2.f);
            }
            // otherWigsInRoom: keep away from the other dropbugs' spots
            rooms::for_each_creature(state, room.id, ::creature::type::dropbug, [&](dcon::creature_id o) {
                if (o != d)
                    num += custom::lerp_map(float_dist(test, w.dropbug_data_get_ceiling(creature::dropbug_data(w, o)).ceiling_pos), 50.f, 1.f, 0.f, 1000.f, 3.f);
            });
            return num;
        }

        // DropBugAI.IdleScore: lower is better. Reachable and possible to get
        // back from is judged by the distance field to where it is
        float idle_score(sys::game_state& state, dcon::creature_id d, room_view const& room, ivec2 coord) {
            if (!accessible(room, coord))
                return float_max;
            ivec2 const at = here(state, d);
            if (pathing::distance(state, pathing::field_to(state, room.id, at, pathing::drop_bug), coord) == pathing::unreachable)
                return float_max;
            float num = 1.f;
            if (narrow_space(room, coord))
                num += 300.f;
            if (room.get_tile(coord).water != 0)
                num += 1000.f;
            if (pathing::tile_access(room, coord) == pathing::access::ceiling)
                num -= 100.f;
            if (solid(room, coord + ivec2{0, 1}))
                num -= 300.f;
            num += mathf::max(0.f, float_dist(at, coord) - 80.f) / 2.f;
            num -= float(std::max(smoothed_floor_altitude(room, coord), 16)) * 2.f;
            num += mathf::max(float_dist(state.world.dropbug_data_get_ceiling(creature::dropbug_data(state.world, d)).ceiling_pos, coord) - 20.f, 0.f);
            return num;
        }

        void lose_all_grasps(sys::game_state& state, dcon::creature_id d) {
            auto& w = state.world;
            if (auto const p = grasped(state, d)) {
                uint8_t& n = w.physical_object_get_grabbed_by_count(player::body(state, p));
                if (n > 0)
                    --n;
            }
            w.creature_get_grasp_player(d)[0] = 0;
        }

        void play_voice(sys::game_state& state, dcon::creature_id d) {
            if (state.world.creature_get_payload(d).dropbug.voice > 0)
                return;
            state.play_sound(sound::sound_id::drop_bug_voice, room_of(state, d), pos(state, d, 0));
            state.world.creature_get_payload(d).dropbug.voice = voice_ticks;
        }

        // DropBug.Jump
        void jump(sys::game_state& state, dcon::creature_id d, vec2 dir) {
            auto& w = state.world;
            float const k = custom::lerp_map(dir.y, -1.f, 1.f, 0.7f, 1.2f, 1.1f);
            w.creature_get_payload(d).dropbug.footing_counter = 0;
            vel(state, d, 0) *= 0.5f;
            vel(state, d, 1) *= 0.5f;
            vel(state, d, 0) += dir * 21.f * k;
            vel(state, d, 1) += dir * 16.f * k;
            w.creature_get_payload(d).dropbug.attempt_bite = 1.f;
            w.creature_get_payload(d).dropbug.charging = 0.f;
            w.creature_get_payload(d).dropbug.jumping = 1;
        }

        // DropBug.JumpFromCeiling: `p`'s chunk `k` as the target, or none
        void jump_from_ceiling(sys::game_state& state, dcon::creature_id d, dcon::player_id p, uint32_t k, vec2 dir) {
            auto& w = state.world;
            w.creature_get_payload(d).dropbug.in_ceiling_mode = 0.f;
            w.creature_get_payload(d).dropbug.from_ceiling_jump = 1;
            jump(state, d, dir);
            w.creature_get_payload(d).dropbug.jump_at_player = p ? uint8_t(p.index() + 1) : 0;
            w.creature_get_payload(d).dropbug.jump_at_chunk = uint8_t(k);
            // (sic) without a target the direction becomes the position to jump at
            if (!p)
                w.dropbug_data_set_jump_at_pos(creature::dropbug_data(w, d), dir);
            // (graphicsModule != null)
            if (state.viewed(room_of(state, d)))
                w.dropbug_data_get_graphics(creature::dropbug_data(w, d)).ceiling_jump = 1;
            state.play_sound(sound::sound_id::drop_bug_drop_from_ceiling, room_of(state, d), pos(state, d, 0));
            play_voice(state, d);
        }

        // DropBug.InitiateJump at a player's chunk
        void initiate_jump(sys::game_state& state, dcon::creature_id d, dcon::player_id p, uint32_t k) {
            auto& w = state.world;
            if (w.creature_get_payload(d).dropbug.charging > 0.f || w.creature_get_payload(d).dropbug.jumping || grasped(state, d) || w.creature_get_payload(d).dropbug.after_drop_attack_delay > 0)
                return;
            w.creature_get_payload(d).dropbug.charging = 0.01f;
            w.creature_get_payload(d).dropbug.jump_at_player = uint8_t(p.index() + 1);
            w.creature_get_payload(d).dropbug.jump_at_chunk = uint8_t(k);
            w.dropbug_data_set_jump_at_pos(creature::dropbug_data(w, d), vec2{0.f, 0.f});
            state.play_sound(sound::sound_id::drop_bug_prepare_jump, room_of(state, d), pos(state, d, 0));
        }

        // the chunk it is jumping at (jumpAtChunk), if any
        std::optional<dcon::body_chunk_id> jump_at_chunk(sys::game_state const& state, dcon::creature_id d) {
            auto const p = player_of(state, state.world.creature_get_payload(d).dropbug.jump_at_player);
            if (!p)
                return std::nullopt;
            return player::body_chunk(state, p, state.world.creature_get_payload(d).dropbug.jump_at_chunk);
        }

        // DropBug.AnticipateDrop
        void anticipate_drop(sys::game_state& state, dcon::creature_id d) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(d);
            if (w.creature_get_payload(d).dropbug.drop_anticipation > 0.f)
                return;
            w.creature_get_payload(d).dropbug.drop_anticipation = 1.f;
            pos(state, d, 0) += custom::rnv(rng) * 2.f;
            // the antennae twitch where a camera looks (graphicsModule != null);
            // the rolls are made either way, so the generator doesn't depend on it
            bool const viewed = state.viewed(room_of(state, d));
            for (auto& a : w.dropbug_data_get_graphics(creature::dropbug_data(w, d)).antennae) {
                vec2 const twitch = custom::rnv(rng) * 7.f * rng.value();
                if (viewed)
                    a.pos += twitch;
            }
        }

        // CeilingSitModule.Dislodge
        void dislodge(sys::game_state& state, dcon::creature_id d, room_view const& room) {
            auto& w = state.world;
            if (!is_sitting_in_ceiling(state, d, room))
                return;
            auto& rng = w.creature_get_random(d);
            auto& c = w.dropbug_data_get_ceiling(creature::dropbug_data(w, d));
            c.stay_away_from_pos = c.ceiling_pos;
            c.ceiling_pos = random_tile(rng, room);
            c.random_attractor = random_tile(rng, room);
            c.win_streak = 0;
            w.dropbug_data_get_utilities(creature::dropbug_data(w, d)).ceiling = 0.f;
            jump_from_ceiling(state, d, dcon::player_id{}, 0, vec2{0.f, -1.f});
        }

        // CeilingSitModule.JumpFromCeiling
        void module_jump_from_ceiling(sys::game_state& state, dcon::creature_id d, room_view const& room, dcon::player_id p, uint32_t k, vec2 dir) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(d);
            jump_from_ceiling(state, d, p, k, dir);
            auto& c = w.dropbug_data_get_ceiling(creature::dropbug_data(w, d));
            c.win_streak = 0;
            c.stay_away_from_pos = c.ceiling_pos;
            c.ceiling_pos = random_tile(rng, room);
            c.random_attractor = random_tile(rng, room);
            w.dropbug_data_set_attack_counter(creature::dropbug_data(w, d), 500);
            w.dropbug_data_get_utilities(creature::dropbug_data(w, d)).prey = 1.f;
            w.dropbug_data_get_utilities(creature::dropbug_data(w, d)).ceiling = 0.f;
            c.drop_delay = 0.f;
            if (p)
                w.dropbug_data_set_target_player(creature::dropbug_data(w, d), uint8_t(p.index() + 1));
        }

        bool holds_spear(sys::game_state const& state, dcon::player_id p) {
            for (uint32_t h = 0; h < 2; ++h) {
                auto const it = items::held(state, p, h);
                if (!it)
                    continue;
                auto const t = state.world.item_get_type(it);
                if (t == world::item_type::spear || t == world::item_type::explosive_spear || t == world::item_type::electric_spear)
                    return true;
            }
            return false;
        }

        // CeilingSitModule.SitUpdate: watching for prey passing under it, and
        // for slugcats with spears in reach
        void sit_update(sys::game_state& state, dcon::creature_id d, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(d);
            auto& c = w.dropbug_data_get_ceiling(creature::dropbug_data(w, d));
            c.last_anticipate_drop = c.anticipate_drop;
            c.anticipate_drop = 0;
            bool spear = false;
            vec2 const head = pos(state, d, 0);
            auto const p = tracked(state, d);
            if (p && w.creature_get_sees_player(d)) {
                vec2 const main = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
                if (std::fabs(head.x - main.x) < 200.f && std::fabs(head.y - main.y) < 30.f && holds_spear(state, p))
                    spear = true;
                // a slugcat is always prey (Eats)
                if (std::fabs(head.x - main.x) < 100.f && head.y > main.y) {
                    uint32_t const k = uint32_t(rng.range(0, 2));
                    auto const kc = player::body_chunk(state, p, k);
                    vec2 const kp = w.body_chunk_get_pos(kc);
                    if (room.visual_contact(head, kp)) {
                        vec2 v = kp + rw::clamp_magnitude(kp - w.body_chunk_get_last_pos(kc), 6.f) * custom::lerp_map(rw::distance(head, kp), 40.f, 500.f, 0.f, 30.f, 0.8f);
                        if (room_view::get_tile_position(kp).x == c.ceiling_pos.x) {
                            if (!room.visual_contact(head, v))
                                v = kp;
                            if (room.visual_contact(head, v)) {
                                c.drop_delay += custom::lerp_map(head.y - v.y, 50.f, 180.f, 0.083333336f, 1.f, 2.f);
                                if (c.drop_delay >= 1.f) {
                                    module_jump_from_ceiling(state, d, room, p, k, custom::dir_vec(head, v));
                                    return;
                                }
                            }
                        } else if (std::abs(room_view::get_tile_position(v).x - c.ceiling_pos.x) < 2) {
                            c.anticipate_drop = 1;
                        }
                    }
                }
            }
            if (c.anticipate_drop && !c.last_anticipate_drop)
                anticipate_drop(state, d);
            if (c.drop_delay > 0.f) {
                c.drop_delay += 0.05f;
                if (c.drop_delay >= 1.f) {
                    module_jump_from_ceiling(state, d, room, dcon::player_id{}, 0, vec2{0.f, -1.f});
                    return;
                }
            }
            if (spear) {
                ++c.spear_danger;
                w.creature_get_payload(d).dropbug.drop_anticipation = mathf::max(w.creature_get_payload(d).dropbug.drop_anticipation, mathf::inverse_lerp(4.f, 20.f, float(c.spear_danger)));
                if (c.spear_danger > 40) {
                    dislodge(state, d, room);
                    w.dropbug_data_set_attack_counter(creature::dropbug_data(w, d), 150);
                }
            } else {
                c.spear_danger = 0;
            }
            // (nothing it tracks is a threat: the ThreatTracker dislodge never happens)
        }

        // CeilingSitModule.Update
        void ceiling_module_update(sys::game_state& state, dcon::creature_id d, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(d);
            auto const& tiles = ceiling_tiles(state, room);
            auto& c = w.dropbug_data_get_ceiling(creature::dropbug_data(w, d));
            if (c.anywhere_to_sit_counter < int32_t(tiles.size())) {
                if (valid_ceiling_spot(state, room, tiles[size_t(c.anywhere_to_sit_counter)])) {
                    c.anywhere_to_sit = 1;
                    c.anywhere_to_sit_counter = int32_t(tiles.size());
                } else {
                    ++c.anywhere_to_sit_counter;
                }
            }
            if (is_sitting_in_ceiling(state, d, room)) {
                sit_update(state, d, room);
                return;
            }
            if (!tiles.empty()) {
                ivec2 const test = tiles[size_t(rng.range(0, int32_t(tiles.size())))];
                if (dynamic_ceiling_spot_score(state, d, room, test) < dynamic_ceiling_spot_score(state, d, room, w.dropbug_data_get_ceiling(creature::dropbug_data(w, d)).ceiling_pos)) {
                    w.dropbug_data_get_ceiling(creature::dropbug_data(w, d)).ceiling_pos = test;
                    w.dropbug_data_get_ceiling(creature::dropbug_data(w, d)).win_streak = 0;
                } else {
                    ++w.dropbug_data_get_ceiling(creature::dropbug_data(w, d)).win_streak;
                }
            }
            if (!valid_ceiling_spot(state, room, w.dropbug_data_get_ceiling(creature::dropbug_data(w, d)).ceiling_pos))
                w.dropbug_data_get_ceiling(creature::dropbug_data(w, d)).win_streak = 0;
        }

        // DropBugAI.ReactToNoise, for the noises of this tick it hears
        // (NoiseTracker hearingSkill 1 sitting, else 0.3)
        void hear_noises(sys::game_state& state, dcon::creature_id d, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(d);
            float const hearing = w.creature_get_payload(d).dropbug.sitting ? 1.f : 0.3f;
            for (auto const& n : state.noises(room.id)) {
                if (!custom::dist_less(n.pos, pos(state, d, 0), n.strength * hearing))
                    continue;
                float const ceiling = w.creature_get_payload(d).dropbug.in_ceiling_mode;
                if (!w.creature_get_payload(d).dropbug.sitting || w.dropbug_data_get_noise_reaction_delay(creature::dropbug_data(w, d)) > 0 || rng.value() < mathf::lerp(1.f, 0.1f, ceiling))
                    return;
                pos(state, d, 1) += custom::rnv(rng) * 4.f;
                auto& g = w.dropbug_data_get_graphics(creature::dropbug_data(w, d));
                // (graphicsModule != null, the rolls made either way)
                for (auto& a : g.antennae) {
                    vec2 const twitch = custom::dir_vec(pos(state, d, 0), n.pos) * rng.value() * mathf::lerp(20.f, 1.f, g.deep_ceiling_mode);
                    if (state.viewed(room.id))
                        a.pos += twitch;
                }
                w.dropbug_data_set_noise_reaction_delay(creature::dropbug_data(w, d), rng.range(0, 30 + int32_t(30.f * ceiling)));
            }
        }

        // DropBugAI.Update (ArtificialIntelligence.Update's modules first)
        void ai_update(sys::game_state& state, dcon::creature_id d, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(d);
            tracker_update(state, d, room);
            stuck_update(state, d);
            // UtilityComparer: threat (1, nothing threatens it here), prey (1,
            // tick 0.01), rain (1), stuck (1), injury (1, tick 1/150),
            // ceiling (lerp 0.2 up, tick 0.005 down)
            auto& u = w.dropbug_data_get_utilities(creature::dropbug_data(w, d));
            auto const tween_tick = [](float a, float b, float speed) { return a < b ? mathf::min(a + speed, b) : mathf::max(a - speed, b); };
            u.prey = tween_tick(u.prey, prey_utility(state, d) * u.prey_weight, 0.01f);
            u.injury = tween_tick(u.injury, injury_utility(state, d), 0.006666667f);
            float const ceiling = ceiling_utility(state, d, room) * u.ceiling_weight;
            u.ceiling = u.ceiling < ceiling ? mathf::lerp(u.ceiling, ceiling, 0.2f) : tween_tick(u.ceiling, ceiling, 0.005f);
            float const stuck = stuck_utility(state, d);
            float utility = 0.f;
            behavior b = behavior::flee;
            auto const consider = [&](float v, behavior k) {
                if (v > utility) {
                    utility = v;
                    b = k;
                }
            };
            consider(u.prey, behavior::hunt);
            consider(dens::rain_utility(state, d, room.id), behavior::escape_rain);
            consider(stuck, behavior::get_unstuck);
            consider(u.injury, behavior::injured);
            consider(u.ceiling, behavior::sit_in_ceiling);
            ceiling_module_update(state, d, room);

            // weights
            int32_t& attack = w.dropbug_data_get_attack_counter(creature::dropbug_data(w, d));
            u.prey_weight = 0.07f + (w.dropbug_data_get_target_player(creature::dropbug_data(w, d)) ? 0.83f : 0.57f) * mathf::inverse_lerp(0.f, 100.f, float(attack));
            u.ceiling_weight = mathf::inverse_lerp(200.f, 0.f, float(attack)) * 0.7f;
            if (attack > 0)
                --attack;
            else
                w.dropbug_data_set_target_player(creature::dropbug_data(w, d), 0);
            if (utility < 0.05f)
                b = behavior::idle;
            if (utility < 0.6f && !anywhere_to_sit_in_room(state, d, room)) {
                utility = 0.6f;
                b = behavior::leave_room;
            }
            // holding prey (a slugcat is always food)
            if (b != behavior::flee && grasped(state, d)) {
                b = behavior::return_prey;
                utility = 1.f;
            }
            ivec2 const at = here(state, d);
            if (b != behavior::idle)
                w.dropbug_data_set_temp_idle_pos(creature::dropbug_data(w, d), at);
            auto const prey = tracked(state, d);
            if (b == behavior::injured && prey_utility(state, d) > 0.4f && prey
            && float_dist(at, room_view::get_tile_position(w.creature_get_last_seen_pos(d))) < 6.f) {
                b = behavior::hunt;
                u.prey_weight = 1.f;
            }
            w.creature_set_behavior(d, uint8_t(b));
            w.creature_set_current_utility(d, utility);

            ivec2& dest = w.creature_get_destination(d);
            switch (b) {
            case behavior::get_unstuck:
                if (rng.value() < 0.02f)
                    dest = room_view::get_tile_position(pos(state, d, 0) + custom::rnv(rng) * 100.f);
                break;
            case behavior::leave_room:
                // another room: a room exit (no migration between rooms)
                if (auto const e = exit_tile(state, room))
                    dest = *e;
                break;
            case behavior::escape_rain:
            case behavior::return_prey:
            case behavior::injured:
                // DenFinder: the den (dens.hpp)
                if (auto const e = dens::home_tile(state, d, room))
                    dest = *e;
                break;
            case behavior::idle: {
                if (idle_score(state, d, room, dest) == float_max)
                    dest = at;
                if (auto const with = w.dropbug_data_get_walk_with_bug(creature::dropbug_data(w, d))) {
                    dest = *with;
                    if (rng.value() < 0.02f || std::abs(at.x - with->x) + std::abs(at.y - with->y) < 4)
                        w.dropbug_data_set_walk_with_bug(creature::dropbug_data(w, d), std::nullopt);
                    break;
                }
                ivec2 const coord = random_tile(rng, room);
                ivec2& temp = w.dropbug_data_get_temp_idle_pos(creature::dropbug_data(w, d));
                if (idle_score(state, d, room, coord) < idle_score(state, d, room, temp))
                    temp = coord;
                int32_t& counter = w.dropbug_data_get_idle_pos_counter(creature::dropbug_data(w, d));
                if (idle_score(state, d, room, temp) < idle_score(state, d, room, dest) + custom::lerp_map(float(counter), 0.f, 300.f, 100.f, -300.f)) {
                    dest = temp;
                    counter = rng.range(200, 800);
                    temp = random_tile(rng, room);
                }
                --counter;
                break;
            }
            case behavior::hunt: {
                if (!prey)
                    break;
                dest = room_view::get_tile_position(w.creature_get_last_seen_pos(d));
                if (grasped(state, d) || w.creature_get_payload(d).dropbug.jumping || w.creature_get_payload(d).dropbug.attempt_bite != 0.f || w.creature_get_payload(d).dropbug.charging != 0.f
                || !footing(state, d) || w.player_get_in_shortcut(prey) || w.physical_object_get_room_from_object_location(player::body(state, prey)) != room.id)
                    break;
                uint32_t const k = uint32_t(rng.range(0, 2));
                vec2 const kp = w.body_chunk_get_pos(player::body_chunk(state, prey, k));
                vec2 const main = pos(state, d, 0), mid = pos(state, d, 1);
                vec2 const behind = mid - custom::dir_vec(mid, kp) * 30.f;
                if (custom::dist_less(main, kp, 120.f) && (accessible(room, behind) || room.get_tile(behind).solid()) && room.visual_contact(main, kp)) {
                    if (rw::dot(rw::normalized(main - kp), rw::normalized(mid - main)) > 0.2f) {
                        initiate_jump(state, d, prey, k);
                    } else {
                        vel(state, d, 0) += custom::dir_vec(main, kp) * 2.f;
                        vel(state, d, 1) -= custom::dir_vec(main, kp) * 2.f;
                    }
                }
                break;
            }
            case behavior::flee:
                // ThreatTracker.FleeTo: nothing it tracks is a threat
                break;
            case behavior::sit_in_ceiling: {
                ivec2 const cp = w.dropbug_data_get_ceiling(creature::dropbug_data(w, d)).ceiling_pos;
                dest = cp;
                vec2 const spot = room_view::middle_of_tile(cp);
                vec2& mid = pos(state, d, 1);
                if (!is_sitting_in_ceiling(state, d, room) && custom::dist_less(mid, spot, 50.f) && !custom::dist_less(mid, spot, 5.f) && room.visual_contact(mid, spot))
                    mid += custom::dir_vec(mid, spot);
                break;
            }
            }
            if (w.dropbug_data_get_noise_reaction_delay(creature::dropbug_data(w, d)) > 0)
                --w.dropbug_data_get_noise_reaction_delay(creature::dropbug_data(w, d));
            hear_noises(state, d, room);
        }

        //
        // DropBug
        //

        // DropBug.MoveBackwards: backing up into its ceiling spot
        bool move_backwards(sys::game_state& state, dcon::creature_id d) {
            auto const& w = state.world;
            return ::dropbug::behavior(w.creature_get_behavior(d)) == behavior::sit_in_ceiling
                && float_dist(w.dropbug_data_get_ceiling(creature::dropbug_data(w, d)).ceiling_pos, here(state, d)) < w.dropbug_data_get_walk_backwards_dist(creature::dropbug_data(w, d))
                && stuck_utility(state, d) == 0.f;
        }

        // Creature.Stun + DropBug.Stun
        void stun(sys::game_state& state, dcon::creature_id d, int32_t st) {
            auto& w = state.world;
            w.creature_set_stun(d, std::max(w.creature_get_stun(d), st));
            if (st > 4 && w.creature_get_random(d).value() < 0.5f)
                lose_all_grasps(state, d);
        }

        // Creature.Die + DropBug.Die
        void die(sys::game_state& state, dcon::creature_id d) {
            state.world.creature_set_dead(d, 1);
            lose_all_grasps(state, d);
        }

        // Creature.Violence on a slugcat (its PlayerState isn't a HealthState):
        // the push, the stun (Player.Stun drops what it holds) and death from
        // a full point of damage (instantDeathDamageLimit 1)
        void hurt_player(sys::game_state& state, dcon::creature_id d, dcon::player_id p, uint32_t k, vec2 momentum, float damage, float stun_bonus) {
            auto& w = state.world;
            auto const c = player::body_chunk(state, p, k);
            w.body_chunk_get_vel(c) += rw::clamp_magnitude(momentum / w.body_chunk_get_mass(c), 10.f);
            int32_t const st = int32_t(damage * 30.f + stun_bonus);
            player::stun(state, p, st);
            if (st > w.creature_get_random(d).range(5, 12))
                items::release_all(state, p);
            if (damage >= 1.f)
                player::die(state, p);
        }

        // the WaterDrips flying off a bite (dark drops)
        void drips(sys::game_state& state, dcon::creature_id d, vec2 a, vec2 b, int n, float speed) {
            auto& rng = state.world.creature_get_random(d);
            for (int i = 0; i < n; ++i)
                particles::spawn(state, room_of(state, d), particles::kind::drop, rw::lerp(a, b, rng.value()), custom::rnv(rng) * rng.value() * speed,
                    1.5f, 0.6f, rw::vec3{0.08f, 0.06f, 0.06f});
        }

        // DropBug.Slash: `k` the chunk it hit, -1 the closest one
        void slash(sys::game_state& state, dcon::creature_id d, dcon::player_id p, int32_t k) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(d);
            vec2 const main = pos(state, d, 0);
            if (k < 0) {
                k = 0;
                float dst = float_max;
                for (uint32_t i = 0; i < 2; ++i) {
                    vec2 const cp = w.body_chunk_get_pos(player::body_chunk(state, p, i));
                    if (custom::dist_less(main, cp, dst)) {
                        dst = rw::distance(main, cp);
                        k = int32_t(i);
                    }
                }
            }
            vec2 const cp = w.body_chunk_get_pos(player::body_chunk(state, p, uint32_t(k)));
            bool const hard = rng.value() < 0.33333334f;
            bool const deep = rng.value() < 0.2f;
            hurt_player(state, d, p, uint32_t(k), custom::dir_vec(main, cp) * 8.f, deep ? 1.1f : 0.4f, hard ? 50.f : 15.f);
            vel(state, d, 0) = custom::dir_vec(cp, main) * 8.f;
            w.creature_get_payload(d).dropbug.grab_on_next_attack = 180;
            drips(state, d, main, cp, 5, deep ? 24.f : 14.f);
            // a slugcat is prey
            if (hard || deep || w.player_get_dead(p))
                w.dropbug_data_set_attack_counter(creature::dropbug_data(w, d), std::max(70, w.dropbug_data_get_attack_counter(creature::dropbug_data(w, d))));
            w.dropbug_data_set_target_player(creature::dropbug_data(w, d), uint8_t(p.index() + 1));
            state.play_sound(sound::sound_id::drop_bug_grab_creature, room_of(state, d), main);
        }

        // DropBug.Attack: the charged up jump
        void attack(sys::game_state& state, dcon::creature_id d, room_view const& room) {
            auto& w = state.world;
            auto const target = jump_at_chunk(state, d);
            vec2 const main = pos(state, d, 0);
            bool const lost = target && (w.physical_object_get_room_from_object_location(player::body(state, player_of(state, w.creature_get_payload(d).dropbug.jump_at_player))) != room.id
                || !room.visual_contact(main, w.body_chunk_get_pos(*target)));
            if (grasped(state, d) || (!target && w.dropbug_data_get_jump_at_pos(creature::dropbug_data(w, d)) == vec2{0.f, 0.f}) || lost) {
                w.creature_get_payload(d).dropbug.charging = 0.f;
                w.creature_get_payload(d).dropbug.jump_at_player = 0;
                return;
            }
            vec2 v = target ? w.body_chunk_get_pos(*target) : w.dropbug_data_get_jump_at_pos(creature::dropbug_data(w, d));
            vec2 const p = v;
            if (!room.get_tile(v + vec2{0.f, 20.f}).solid())
                v += vec2{0.f, mathf::inverse_lerp(40.f, 200.f, rw::distance(main, v)) * 20.f};
            vec2 dir = custom::dir_vec(main, v);
            if (!custom::dist_less(main, p, custom::lerp_map(rw::dot(dir, custom::dir_vec(pos(state, d, 1), main)), -0.1f, 0.8f, 0.f, 300.f, 0.4f))) {
                w.creature_get_payload(d).dropbug.charging = 0.f;
                w.creature_get_payload(d).dropbug.jump_at_player = 0;
                return;
            }
            if (!room.get_tile(main + vec2{0.f, 20.f}).solid() && !room.get_tile(pos(state, d, 1) + vec2{0.f, 20.f}).solid())
                dir = rw::slerp(dir, vec2{0.f, 1.f}, custom::lerp_map(rw::distance(main, v), 40.f, 200.f, 0.05f, 0.2f));
            state.play_sound(sound::sound_id::drop_bug_jump, room.id, main);
            play_voice(state, d);
            jump(state, d, dir);
        }

        // DropBug.MoveTowards
        void move_towards(sys::game_state& state, dcon::creature_id d, room_view const& room, vec2 move_to) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(d);
            if (rng.value() > w.creature_get_health(d))
                return;
            vec2 dir = custom::dir_vec(pos(state, d, 0), move_to);
            if (!footing(state, d))
                dir *= 0.3f;
            bool const backwards = move_backwards(state, d);
            if (!backwards && physics::is_tile_solid(state, room, chunk(state, d, 1), 0, -1)
            && ((dir.x < -0.5f && pos(state, d, 0).x > pos(state, d, 1).x + 5.f) || (dir.x > 0.5f && pos(state, d, 0).x < pos(state, d, 1).x - 5.f))) {
                // turning around on the floor: the head goes over the body
                float const s = dir.x < 0.f ? -1.f : 1.f;
                vel(state, d, 0).x -= s * 1.3f;
                vel(state, d, 1).x += s * 0.5f;
                if (!physics::is_tile_solid(state, room, chunk(state, d, 0), 0, 1))
                    vel(state, d, 0).y += 3.2f;
            }
            float const k = custom::lerp_map(w.dropbug_data_get_carry_object_mass(creature::dropbug_data(w, d)), 0.f, 4.f, 1.f, 0.2f, 0.7f) * mathf::lerp(1.f, 1.5f, w.dropbug_data_get_stuck_shake(creature::dropbug_data(w, d)));
            if (backwards) {
                vel(state, d, 2) += dir * 7.5f * k;
                vel(state, d, 1) += dir * 0.2f * k;
                vel(state, d, 0) -= dir * 0.45f * k;
                physics::set_go_through_floors(state, body(state, d), move_to.y < pos(state, d, 2).y - 5.f);
                return;
            }
            vel(state, d, 0) += dir * 4.5f * k;
            vel(state, d, 1) -= dir * 0.45f * k;
            vel(state, d, 2) -= dir * 0.2f * k;
            physics::set_go_through_floors(state, body(state, d), move_to.y < pos(state, d, 0).y - 5.f);
        }

        // StandardPather.FollowPath from the tile of `p`
        movement_connection follow_path(sys::game_state const& state, pathing::field_id f, ivec2 from) {
            auto const step = pathing::next_step(state, f, from);
            if (!step)
                return movement_connection{};
            return movement_connection{from, *step, true};
        }

        // DropBug.Run. The path fields' steps are Standard moves, OpenDiagonals
        // (a special move) and drops (DropToFloor); no shortcuts or reaches
        void run(sys::game_state& state, dcon::creature_id d, room_view const& room, pathing::field_id f, movement_connection const& following) {
            auto& w = state.world;
            ivec2 const step = following.dest - following.start;
            if (step.x != 0 && step.y != 0) {
                w.dropbug_data_set_special_move_counter(creature::dropbug_data(w, d), 30);
                w.dropbug_data_set_special_move_destination(creature::dropbug_data(w, d), following.dest);
            } else {
                movement_connection conn = following;
                if (stuck_utility(state, d) == 0.f) {
                    // look one step further and cut the corner if it is all walkable
                    auto const next = follow_path(state, f, following.dest);
                    if (next.valid && next.dest == following.start) {
                        w.creature_get_payload(d).dropbug.sitting = 1;
                        return;
                    }
                    if (next.valid && pathing::tile_access(room, next.dest) < pathing::access::ceiling) {
                        bool blocked = false;
                        for (int32_t x = std::min(following.start.x, next.dest.x); x < std::max(following.start.x, next.dest.x) && !blocked; ++x)
                            for (int32_t y = std::min(following.start.y, next.dest.y); y < std::max(following.start.y, next.dest.y); ++y)
                                if (!accessible(room, ivec2{x, y})) {
                                    blocked = true;
                                    break;
                                }
                        if (!blocked)
                            conn = next;
                    }
                }
                bool const backwards = move_backwards(state, d);
                uint32_t const lead = backwards ? 2 : 0;
                vec2 const target = room_view::middle_of_tile(conn.dest);
                w.creature_get_payload(d).dropbug.travel_dir = rw::lerp(w.creature_get_payload(d).dropbug.travel_dir, custom::dir_vec(pos(state, d, lead), target), 0.4f);
                if (footing(state, d)) {
                    for (uint32_t k = 0; k < 3; ++k) {
                        if (following.start.x == following.dest.x)
                            vel(state, d, k).x += mathf::min((target.x - pos(state, d, k).x) / 8.f, 1.2f);
                        else if (following.start.y == following.dest.y)
                            vel(state, d, k).y += mathf::min((target.y - pos(state, d, k).y) / 8.f, 1.2f);
                    }
                }
                auto const& last = w.dropbug_data_get_last_followed_connection(creature::dropbug_data(w, d));
                if (last.valid && (footing(state, d) || accessible(room, pos(state, d, lead)))
                && ((following.start.x != following.dest.x && last.start.x == last.dest.x) || (following.start.y != following.dest.y && last.start.y == last.dest.y))) {
                    vel(state, d, lead) *= 0.7f;
                    vel(state, d, backwards ? 0 : 2) *= 0.5f;
                }
                if (step.y < -1)
                    w.creature_get_payload(d).dropbug.footing_counter = 0; // DropToFloor
                move_towards(state, d, room, target);
            }
            w.dropbug_data_set_last_followed_connection(creature::dropbug_data(w, d), following);
        }

        // DropBug.Swim
        void swim(sys::game_state& state, dcon::creature_id d, room_view const& room, pathing::field_id f) {
            auto& w = state.world;
            vel(state, d, 0) *= 1.f - 0.1f * physics::submersion(state, room, chunk(state, d, 0));
            vel(state, d, 1) *= 1.f - 0.2f * physics::submersion(state, room, chunk(state, d, 1));
            w.creature_get_payload(d).dropbug.run_cycle += 0.125f;
            physics::set_go_through_floors(state, body(state, d), true);
            auto conn = follow_path(state, f, room_view::get_tile_position(pos(state, d, 0)));
            if (!conn.valid)
                conn = follow_path(state, f, room_view::get_tile_position(pos(state, d, 1)));
            ivec2 const at = here(state, d);
            if (!conn.valid && std::abs(at.y - room.default_water_level) < 4)
                conn = follow_path(state, f, ivec2{at.x, room.default_water_level});
            if (!conn.valid) {
                vel(state, d, 0).y += 0.5f;
                return;
            }
            float const s0 = physics::submersion(state, room, chunk(state, d, 0));
            if (conn.start.y == conn.dest.y && conn.dest.y == room.default_water_level) {
                float const side = mathf::sign(room_view::middle_of_tile(conn.start).x - room_view::middle_of_tile(conn.dest).x);
                vel(state, d, 0).x -= side * 1.2f * s0;
                vel(state, d, 1).x += side * 0.5f * physics::submersion(state, room, chunk(state, d, 1));
                w.creature_get_payload(d).dropbug.footing_counter = 0;
                return;
            }
            vel(state, d, 0) *= 0.8f;
            vel(state, d, 0) += custom::dir_vec(pos(state, d, 0), room_view::middle_of_tile(conn.dest)) * 1.4f;
            w.creature_get_payload(d).dropbug.footing_counter = std::max(w.creature_get_payload(d).dropbug.footing_counter, 25);
            run(state, d, room, f, conn);
            w.creature_get_payload(d).dropbug.out_of_water_footing = 20;
        }

        // DropBug.Act
        void act(sys::game_state& state, dcon::creature_id d, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(d);
            ai_update(state, d, room);
            auto const f = pathing::field_to(state, room.id, w.creature_get_destination(d), pathing::drop_bug);
            if (physics::object_submersion(state, room, body(state, d)) > 0.3f) {
                swim(state, d, room, f);
                w.creature_get_payload(d).dropbug.swimming = 1;
                return;
            }
            w.creature_get_payload(d).dropbug.swimming = 0;
            if (rng.value() < 0.005f)
                w.dropbug_data_set_walk_backwards_dist(creature::dropbug_data(w, d), rng.value() * 20.f);

            float& ceiling = w.creature_get_payload(d).dropbug.in_ceiling_mode;
            if (is_sitting_in_ceiling(state, d, room)) {
                // tucked into the ceiling: head down, body and tail up against it
                ceiling = mathf::min(1.f, ceiling + 0.025f);
                vec2 const a = room_view::middle_of_tile(w.dropbug_data_get_ceiling(creature::dropbug_data(w, d)).ceiling_pos);
                pos(state, d, 0) = rw::lerp(pos(state, d, 0), a + vec2{0.f, -5.f * ceiling}, 0.05f * ceiling);
                pos(state, d, 1) = rw::lerp(pos(state, d, 1), a + vec2{0.f, 9.f * ceiling}, 0.4f * ceiling);
                pos(state, d, 2) = rw::lerp(pos(state, d, 2), a + vec2{0.f, 10.f * ceiling}, 0.5f * ceiling);
                for (uint32_t i = 0; i < 3; ++i)
                    vel(state, d, i) *= 1.f - ceiling;
                w.creature_get_payload(d).dropbug.footing_counter = 20;
                w.creature_get_payload(d).dropbug.sitting = 1;
                int32_t& lured = w.dropbug_data_get_lured_to_drop_counter(creature::dropbug_data(w, d));
                if (lured > 0) {
                    --lured;
                    w.creature_get_payload(d).dropbug.drop_anticipation = mathf::lerp(w.creature_get_payload(d).dropbug.drop_anticipation, rng.value(), 0.2f);
                    if (lured < 1)
                        dislodge(state, d, room);
                }
                return;
            }
            ceiling = 0.f;
            w.dropbug_data_set_lured_to_drop_counter(creature::dropbug_data(w, d), 0);

            if (w.creature_get_payload(d).dropbug.jumping) {
                bool hold = false;
                for (uint32_t i = 0; i < 3; ++i)
                    if (w.body_chunk_get_contact_point(chunk(state, d, i)) != ivec2{0, 0} && accessible(room, pos(state, d, i)))
                        hold = true;
                if (hold)
                    ++w.creature_get_payload(d).dropbug.footing_counter;
                else
                    w.creature_get_payload(d).dropbug.footing_counter = 0;
                // steering in the air towards what it jumped at
                auto const target = jump_at_chunk(state, d);
                vec2 const jump_pos = w.dropbug_data_get_jump_at_pos(creature::dropbug_data(w, d));
                if ((target && room.visual_contact(pos(state, d, 0), w.body_chunk_get_pos(*target))) || (!target && jump_pos != vec2{0.f, 0.f})) {
                    vec2 const at = target ? w.body_chunk_get_pos(*target) : jump_pos;
                    vec2 const dir = custom::dir_vec(pos(state, d, 0), at);
                    vel(state, d, 0) += dir * 1.2f;
                    vel(state, d, 1) -= dir * 0.4f;
                    vel(state, d, 2) -= dir * 0.4f;
                    if (w.creature_get_payload(d).dropbug.from_ceiling_jump && pos(state, d, 0).y > at.y + 250.f && custom::dist_less(pos(state, d, 0), at, 350.f)) {
                        vec2 const lead = target ? at + w.body_chunk_get_vel(*target) : at;
                        vel(state, d, 0).x += custom::dir_vec(pos(state, d, 0), lead).x * 3.f;
                    }
                }
                if (footing(state, d)) {
                    w.creature_get_payload(d).dropbug.jumping = 0;
                    w.creature_get_payload(d).dropbug.jump_at_player = 0;
                }
                return;
            }

            float& shake = w.dropbug_data_get_stuck_shake(creature::dropbug_data(w, d));
            float const stuck = stuck_utility(state, d);
            if (stuck > 0.9f)
                shake = custom::lerp_and_tick(shake, 1.f, 0.07f, 0.014285714f);
            else if (stuck < 0.2f)
                shake = custom::lerp_and_tick(shake, 0.f, 0.07f, 0.05f);
            if (shake > 0.f)
                for (uint32_t i = 0; i < 3; ++i) {
                    vel(state, d, i) += custom::rnv(rng) * rng.value() * 5.f * shake;
                    pos(state, d, i) += custom::rnv(rng) * rng.value() * 5.f * shake;
                }

            int32_t& special = w.dropbug_data_get_special_move_counter(creature::dropbug_data(w, d));
            if (special > 0) {
                --special;
                vec2 const to = room_view::middle_of_tile(w.dropbug_data_get_special_move_destination(creature::dropbug_data(w, d)));
                move_towards(state, d, room, to);
                w.creature_get_payload(d).dropbug.travel_dir = rw::lerp(w.creature_get_payload(d).dropbug.travel_dir, custom::dir_vec(pos(state, d, 0), to), 0.4f);
                if (custom::dist_less(pos(state, d, 0), to, 5.f))
                    special = 0;
            } else {
                if (!accessible(room, pos(state, d, 0)) && !accessible(room, pos(state, d, 1)))
                    w.creature_get_payload(d).dropbug.footing_counter = custom::int_clamp(w.creature_get_payload(d).dropbug.footing_counter - 3, 0, 35);
                ivec2 const dest = w.creature_get_destination(d);
                float& charging = w.creature_get_payload(d).dropbug.charging;
                if (footing(state, d) && charging > 0.f) {
                    // crouching for the jump
                    w.creature_get_payload(d).dropbug.sitting = 1;
                    physics::set_go_through_floors(state, body(state, d), false);
                    charging += 0.06666667f;
                    std::optional<vec2> dir;
                    if (w.dropbug_data_get_jump_at_pos(creature::dropbug_data(w, d)) != vec2{0.f, 0.f})
                        dir = custom::dir_vec(pos(state, d, 0), w.dropbug_data_get_jump_at_pos(creature::dropbug_data(w, d)));
                    if (auto const target = jump_at_chunk(state, d))
                        dir = custom::dir_vec(pos(state, d, 0), w.body_chunk_get_pos(*target));
                    if (dir) {
                        vel(state, d, 0) += *dir * mathf::pow(charging, 2.f);
                        vel(state, d, 1) -= *dir * 4.f * charging;
                    }
                    if (charging >= 1.f)
                        attack(state, d, room);
                } else if (room_view::get_tile_position(pos(state, d, 0)) == dest || room_view::get_tile_position(pos(state, d, 1)) == dest) {
                    // there (the threat tracker's utility is always 0 here)
                    w.creature_get_payload(d).dropbug.sitting = 1;
                    physics::set_go_through_floors(state, body(state, d), false);
                } else {
                    bool const backwards = move_backwards(state, d);
                    auto conn = follow_path(state, f, room_view::get_tile_position(pos(state, d, backwards ? 2 : 0)));
                    if (!conn.valid)
                        conn = follow_path(state, f, room_view::get_tile_position(pos(state, d, backwards ? 0 : 2)));
                    if (!conn.valid)
                        conn = follow_path(state, f, room_view::get_tile_position(pos(state, d, 1)));
                    if (conn.valid)
                        run(state, d, room, f, conn);
                    else
                        physics::set_go_through_floors(state, body(state, d), false);
                }
            }
            float const cycle = w.creature_get_payload(d).dropbug.run_cycle;
            auto const head = chunk(state, d, 0);
            if (!custom::dist_less(w.body_chunk_get_pos(head), w.body_chunk_get_last_pos(head), 2.f))
                w.creature_get_payload(d).dropbug.run_cycle += 0.125f;
            if (cycle < std::floor(w.creature_get_payload(d).dropbug.run_cycle))
                state.play_sound(sound::sound_id::drop_bug_step, room.id, w.body_chunk_get_pos(head));
        }

        // DropBug.CarryObject: holding a slugcat in its mandibles
        void carry_object(sys::game_state& state, dcon::creature_id d) {
            auto& w = state.world;
            auto const p = grasped(state, d);
            if (w.player_get_in_shortcut(p) || w.physical_object_get_room_from_object_location(player::body(state, p)) != room_of(state, d)) {
                lose_all_grasps(state, d);
                return;
            }
            uint32_t const k = w.creature_get_grasp_chunk(d)[0];
            auto const gc = player::body_chunk(state, p, k);
            float const theirs = object_mass(state, player::body(state, p));
            float carry = theirs;
            if (carry <= total_mass(state, d) * 1.1f)
                carry /= 2.f;
            else if (carry <= total_mass(state, d) / 5.f)
                carry = 0.f;
            w.dropbug_data_set_carry_object_mass(creature::dropbug_data(w, d), carry);
            auto const main = chunk(state, d, 0);
            vec2& mp = w.body_chunk_get_pos(main);
            vec2& gp = w.body_chunk_get_pos(gc);
            float const num = w.body_chunk_get_rad(main) + w.body_chunk_get_rad(gc);
            vec2 const a = -custom::dir_vec(mp, gp) * (num - rw::distance(mp, gp));
            float ratio = w.body_chunk_get_mass(gc) / (w.body_chunk_get_mass(main) + w.body_chunk_get_mass(gc));
            ratio *= 0.2f * (1.f - stuck_utility(state, d));
            mp += a * ratio;
            w.body_chunk_get_vel(main) += a * ratio;
            gp -= a * (1.f - ratio);
            w.body_chunk_get_vel(gc) -= a * (1.f - ratio);
            vec2 const hold = mp + custom::dir_vec(pos(state, d, 1), mp) * num;
            vec2 const rel = w.body_chunk_get_vel(gc) - w.body_chunk_get_vel(main);
            w.body_chunk_set_vel(gc, w.body_chunk_get_vel(main));
            if (rw::magnitude(rel) * w.body_chunk_get_mass(gc) > 30.f || !custom::dist_less(hold, gp, 70.f + w.body_chunk_get_rad(gc))) {
                lose_all_grasps(state, d);
                return;
            }
            gp = hold;
            // PhysicalObject.PushOutOf: the slugcat's other chunk out of the bug
            for (uint32_t i = 0; i < 2; ++i) {
                vec2 const bp = pos(state, d, i);
                float const rad = w.body_chunk_get_rad(chunk(state, d, i));
                auto const other = player::body_chunk(state, p, 1 - k);
                vec2& op = w.body_chunk_get_pos(other);
                float const r = rad + w.body_chunk_get_rad(other);
                if (custom::dist_less(op, bp, r)) {
                    float const dist = rw::distance(op, bp);
                    vec2 const dir = custom::dir_vec(bp, op);
                    op += dir * (r - dist);
                    w.body_chunk_get_vel(other) += dir * (r - dist);
                }
            }
        }


        // DropBug.Collide with a slugcat: myChunk `i`, their chunk `j`
        void collide_with_player(sys::game_state& state, dcon::creature_id d, uint32_t i, dcon::player_id p, uint32_t j) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(d);
            // tracker.SeeCreature
            w.creature_set_seen_player(d, uint8_t(p.index() + 1));
            w.creature_set_ticks_since_seen(d, 0);
            w.creature_set_last_seen_pos(d, w.body_chunk_get_pos(player::body_chunk(state, p, 0)));
            float const bite = w.creature_get_payload(d).dropbug.attempt_bite;
            bool const from_ceiling = w.creature_get_payload(d).dropbug.from_ceiling_jump;
            bool const jumping = w.creature_get_payload(d).dropbug.jumping;
            auto const kc = player::body_chunk(state, p, j);
            vec2 const kp = w.body_chunk_get_pos(kc);
            vec2 const main = pos(state, d, 0);
            if (i != 0 || grasped(state, d) || bite <= 0.f || !(jumping || from_ceiling || bite > 0.75f))
                return;
            if (!((from_ceiling && pos(state, d, 2).y > kp.y) || rw::dot(custom::dir_vec(pos(state, d, 1), main), custom::dir_vec(main, kp)) > mathf::lerp(0.7f, -0.2f, bite)))
                return;
            if (jumping)
                vel(state, d, 0) *= 0.5f;
            drips(state, d, main, kp, 4, 14.f);
            if (w.creature_get_payload(d).dropbug.grab_on_next_attack > 0 || w.player_get_dead(p) || rng.value() > (from_ceiling ? 0.8f : 0.2f)) {
                // Creature.Grab, then the bite
                w.creature_get_grasp_player(d)[0] = uint8_t(p.index() + 1);
                w.creature_get_grasp_chunk(d)[0] = uint8_t(j);
                ++w.physical_object_get_grabbed_by_count(player::body(state, p));
                hurt_player(state, d, p, j, custom::dir_vec(main, kp) * 4.f, from_ceiling && rng.value() < 0.2f ? 1.1f : 0.4f, 0.f);
                state.play_sound(sound::sound_id::drop_bug_grab_creature, room_of(state, d), main);
                if (w.creature_get_payload(d).dropbug.grab_on_next_attack < 1)
                    w.creature_get_payload(d).dropbug.release_grabbed_counter = 35;
                w.creature_get_payload(d).dropbug.grab_on_next_attack = 0;
            } else {
                slash(state, d, p, int32_t(j));
            }
            w.creature_get_payload(d).dropbug.attempt_bite = 0.f;
            w.creature_get_payload(d).dropbug.charging = 0.f;
            w.creature_get_payload(d).dropbug.jumping = 0;
            w.creature_get_payload(d).dropbug.from_ceiling_jump = 0;
        }

        // DropBugAI.CollideWithKin
        void collide_with_kin(sys::game_state& state, dcon::creature_id d, dcon::creature_id other) {
            auto& w = state.world;
            ivec2 const at = here(state, d);
            ivec2 const theirs = w.creature_get_destination(other);
            if (std::abs(at.x - theirs.x) + std::abs(at.y - theirs.y) < 4)
                return;
            if ((w.dropbug_data_get_dominance(creature::dropbug_data(w, other)) > w.dropbug_data_get_dominance(creature::dropbug_data(w, d)) && !w.creature_get_payload(other).dropbug.sitting) || w.creature_get_payload(d).dropbug.sitting)
                w.dropbug_data_set_walk_with_bug(creature::dropbug_data(w, d), theirs);
            auto const room = world::get_room_view(state, room_of(state, d));
            if (is_sitting_in_ceiling(state, d, room) && w.dropbug_data_get_dominance(creature::dropbug_data(w, d)) < w.dropbug_data_get_dominance(creature::dropbug_data(w, other))
            && float_dist(w.dropbug_data_get_ceiling(creature::dropbug_data(w, d)).ceiling_pos, w.dropbug_data_get_ceiling(creature::dropbug_data(w, other)).ceiling_pos) < 3.f)
                dislodge(state, d, room);
        }


        // DropBug.PlaceInRoom's search for the best ceiling spot of the room
        std::optional<ivec2> best_ceiling_spot(sys::game_state& state, dcon::creature_id d, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(d);
            auto const& tiles = ceiling_tiles(state, room);
            if (tiles.empty())
                return std::nullopt;
            std::vector<ivec2> list;
            for (int i = 0; i < 100; ++i) {
                ivec2 const t = tiles[size_t(rng.range(0, int32_t(tiles.size())))];
                if (valid_ceiling_spot(state, room, t)) {
                    list.push_back(t);
                    if (list.size() > 10)
                        break;
                }
            }
            if (list.empty())
                return std::nullopt;
            auto const& c = w.dropbug_data_get_ceiling(creature::dropbug_data(w, d));
            ivec2 best = list[size_t(rng.range(0, int32_t(list.size())))];
            float lowest = float_max;
            for (ivec2 const t : list) {
                float score = ceiling_spot_score(state, room, t);
                score += custom::lerp_map(float_dist(t, c.stay_away_from_pos), 0.f, 15.f, 400.f, 0.f);
                score -= custom::lerp_map(float_dist(t, c.random_attractor), float(c.room_size / 4), float(c.room_size / 2), 150.f, 0.f);
                // the creatures already in the room, other dropbugs most of all
                rooms::for_each_creature(state, room.id, ::creature::type::dropbug, [&](dcon::creature_id o) {
                    if (o == d)
                        return;
                    float const dist = float_dist(t, here(state, o));
                    score += custom::lerp_map(dist, 50.f, 2.f, 0.f, 500.f, 3.f);
                    score += custom::lerp_map(dist, 1.f, 9.f, 200.f, 0.f, 0.5f);
                });
                if (score < lowest) {
                    best = t;
                    lowest = score;
                }
            }
            return best;
        }
    }

    // DropBug.Update
    void update(sys::game_state& state, dcon::creature_id d, room_view const& room) {
        auto& w = state.world;
        auto& rng = w.creature_get_random(d);
        auto const o = body(state, d);
        if (!w.creature_get_dead(d)) {
            float& health = w.creature_get_health(d);
            if (health < 0.f && rng.value() < -health && rng.value() < 1.f / float(consious(state, d) ? 80 : 800))
                die(state, d);
            if (rng.value() < 0.033333335f && (rng.value() * 0.2f > health || rng.value() < -health))
                stun(state, d, rng.range(1, rng.range(1, 27 - custom::int_clamp(int32_t(20.f * health), 0, 10))));
            if (health > 0.f && health < 1.f && rng.value() < 0.01f)
                health = mathf::min(1.f, health + 1.f / mathf::lerp(550.f, 70.f, health));
            if (w.creature_get_payload(d).dropbug.grab_on_next_attack > 0)
                --w.creature_get_payload(d).dropbug.grab_on_next_attack;
            if (!consious(state, d) && rng.value() < 0.05f)
                vel(state, d, 1) += custom::rnv(rng) * rng.value() * 3.f;
            // (slugcats can't grab creatures: grabbedCounter stays 0)
            int32_t& release = w.creature_get_payload(d).dropbug.release_grabbed_counter;
            if (release > 0) {
                --release;
                auto const p = grasped(state, d);
                if (p && !w.player_get_dead(p) && object_mass(state, player::body(state, p)) > total_mass(state, d) * 0.3f) {
                    // worrying the prey, then a last slash and let go
                    auto const gc = player::body_chunk(state, p, w.creature_get_grasp_chunk(d)[0]);
                    vec2 const a = custom::rnv(rng);
                    pos(state, d, 0) += a * 4.f;
                    vel(state, d, 0) += a * 4.f;
                    w.body_chunk_get_pos(gc) += rw::clamp_magnitude(a * 2.f / w.body_chunk_get_mass(gc), 5.f);
                    w.body_chunk_get_vel(gc) += rw::clamp_magnitude(a * 2.f / w.body_chunk_get_mass(gc), 7.f);
                    if (release == 1) {
                        slash(state, d, p, -1);
                        lose_all_grasps(state, d);
                    }
                } else {
                    release = 0;
                }
            }
        }
        if (w.creature_get_payload(d).dropbug.out_of_water_footing > 0)
            --w.creature_get_payload(d).dropbug.out_of_water_footing;
        w.creature_get_payload(d).dropbug.attempt_bite = mathf::max(0.f, w.creature_get_payload(d).dropbug.attempt_bite - 0.025f);
        if (w.creature_get_payload(d).dropbug.from_ceiling_jump) {
            w.creature_get_payload(d).dropbug.attempt_bite = 1.f;
            for (uint32_t i = 0; i < 3; ++i) {
                if (w.body_chunk_get_contact_point(chunk(state, d, i)).y < 0 || physics::submersion(state, room, chunk(state, d, i)) > 0.5f) {
                    // landed without catching anything
                    w.creature_get_payload(d).dropbug.after_drop_attack_delay = 20;
                    w.creature_get_payload(d).dropbug.from_ceiling_jump = 0;
                    w.creature_get_payload(d).dropbug.attempt_bite = 0.f;
                    w.creature_get_payload(d).dropbug.jumping = 0;
                    w.creature_get_payload(d).dropbug.jump_at_player = 0;
                    w.creature_get_payload(d).dropbug.charging = 0.f;
                    break;
                }
            }
        }
        // Creature.Update + PhysicalObject.Update
        creature::stun_countdown(state, d);
        if (w.creature_get_payload(d).dropbug.voice > 0)
            --w.creature_get_payload(d).dropbug.voice;
        physics::update_object(state, room, o, nullptr, nullptr);
        w.creature_get_payload(d).dropbug.drop_anticipation = mathf::max(w.creature_get_payload(d).dropbug.drop_anticipation - 0.008333334f, 0.f);
        if (w.creature_get_payload(d).dropbug.after_drop_attack_delay > 0)
            --w.creature_get_payload(d).dropbug.after_drop_attack_delay;
        vec2 const spine = custom::dir_vec(pos(state, d, 2), pos(state, d, 0));
        vel(state, d, 0) += spine * 0.5f;
        vel(state, d, 2) -= spine * 1.f;

        w.creature_get_payload(d).dropbug.sitting = 0;
        if (consious(state, d)) {
            if (accessible(room, pos(state, d, 0)) || accessible(room, pos(state, d, 1)))
                ++w.creature_get_payload(d).dropbug.footing_counter;
            act(state, d, room);
        } else {
            w.creature_get_payload(d).dropbug.footing_counter = 0;
            w.creature_get_payload(d).dropbug.charging = 0.f;
            w.creature_get_payload(d).dropbug.jumping = 0;
            w.creature_get_payload(d).dropbug.from_ceiling_jump = 0;
            w.creature_get_payload(d).dropbug.in_ceiling_mode = 0.f;
        }
        // clinging on: gravity cancelled (PhysicalObject.gravity, not the room's)
        float const g = w.physical_object_get_g(o);
        if (footing(state, d)) {
            for (uint32_t i = 0; i < 2; ++i) {
                vel(state, d, i) *= 0.8f;
                vel(state, d, i).y += g;
            }
            if (move_backwards(state, d)) {
                vel(state, d, 2) *= 0.8f;
                vel(state, d, 2).y += g;
            } else {
                vel(state, d, 2).y += g * mathf::lerp(0.5f, 1.f, stuck_utility(state, d));
            }
        }
        w.creature_get_payload(d).dropbug.travel_dir = w.creature_get_payload(d).dropbug.travel_dir * (w.creature_get_payload(d).dropbug.sitting ? 0.5f : 0.9995f);
        float const ceiling = w.creature_get_payload(d).dropbug.in_ceiling_mode;
        w.body_chunk_set_collide_with_terrain(chunk(state, d, 2), ceiling < 0.5f);
        w.body_chunk_set_collide_with_terrain(chunk(state, d, 1), ceiling < 0.5f);
        // the body folds up into the ceiling
        w.physical_object_for_each_chunk_connection_as_owner(o, [&](dcon::chunk_connection_id k) {
            uint8_t const a = w.body_chunk_get_index(w.chunk_connection_get_chunk1(k));
            uint8_t const b = w.body_chunk_get_index(w.chunk_connection_get_chunk2(k));
            if (a == 0 && b == 1)
                w.chunk_connection_set_distance(k, mathf::lerp(12.f, 5.f, ceiling));
            else if (a == 1 && b == 2)
                w.chunk_connection_set_distance(k, mathf::lerp(14.f, 2.f, ceiling));
            else
                w.chunk_connection_set_distance(k, mathf::lerp(8.f, 0.f, ceiling));
        });
        if (grasped(state, d))
            carry_object(state, d);
        else
            w.dropbug_data_set_carry_object_mass(creature::dropbug_data(w, d), 0.f);
    }

    bool footing(sys::game_state const& state, dcon::creature_id d) noexcept {
        return state.world.creature_get_payload(d).dropbug.footing_counter > 10 || state.world.creature_get_payload(d).dropbug.out_of_water_footing > 0;
    }

    bool sitting_in_ceiling(sys::game_state const& state, dcon::creature_id d) noexcept {
        return is_sitting_in_ceiling(state, d, world::get_room_view(state, room_of(state, d)));
    }

    dcon::player_id grasped(sys::game_state const& state, dcon::creature_id d) noexcept {
        return player_of(state, state.world.creature_get_grasp_player(d)[0]);
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 at, bool relocate) {
        auto& w = state.world;
        auto const d = creature::make(state, creature::type::dropbug);
        rw::rng rng{state.random.next() | 1u};
        auto const view = world::get_room_view(state, room);
        physics::object_desc desc;
        desc.air_friction = 0.999f;
        desc.gravity = 0.9f;
        desc.bounce = 0.1f;
        desc.surface_friction = 0.4f;
        desc.water_friction = 0.96f;
        desc.buoyancy = 0.95f;
        auto const o = physics::create_object(state, room, desc);
        // never inside terrain
        if (view.get_tile(at).solid()) {
            ivec2 const t0 = room_view::get_tile_position(at);
            for (int r = 1; r < 8 && view.get_tile(at).solid(); ++r)
                for (int dy = -r; dy <= r; ++dy)
                    for (int dx = -r; dx <= r; ++dx)
                        if (view.get_tile(at).solid() && !view.get_tile(t0 + ivec2{dx, dy}).solid())
                            at = room_view::middle_of_tile(t0 + ivec2{dx, dy});
        }
        constexpr float mass = 0.8f;
        auto const c0 = physics::add_chunk(state, o, at, 6.f, mass * 0.4f);
        auto const c1 = physics::add_chunk(state, o, at + vec2{0.f, 1.f}, 8.f, mass * 0.4f);
        auto const c2 = physics::add_chunk(state, o, at + vec2{0.f, 2.f}, 6.f, mass * 0.2f);
        physics::add_connection(state, o, c0, c1, 12.f, physics::connection_type::normal, 1.f, -1.f);
        physics::add_connection(state, o, c1, c2, 14.f, physics::connection_type::normal, 1.f, -1.f);
        physics::add_connection(state, o, c0, c2, 8.f, physics::connection_type::push, 1.f, -1.f);
        w.try_create_creature_body(d, o);
        w.creature_set_random(d, rng);
        w.dropbug_data_set_dominance(creature::dropbug_data(w, d), rng.value());
        w.creature_set_stun(d, 0);
        w.creature_set_dead(d, 0);
        w.creature_set_health(d, 1.f);
        w.creature_get_payload(d).dropbug.footing_counter = 0;
        w.creature_get_payload(d).dropbug.out_of_water_footing = 0;
        w.dropbug_data_set_special_move_counter(creature::dropbug_data(w, d), 0);
        w.dropbug_data_set_special_move_destination(creature::dropbug_data(w, d), ivec2{0, 0});
        w.dropbug_data_set_last_followed_connection(creature::dropbug_data(w, d), movement_connection{});
        w.creature_get_payload(d).dropbug.run_cycle = 0.f;
        w.creature_get_payload(d).dropbug.sitting = 0;
        w.creature_get_payload(d).dropbug.swimming = 0;
        w.creature_get_payload(d).dropbug.travel_dir = vec2{0.f, 0.f};
        w.creature_get_payload(d).dropbug.charging = 0.f;
        w.creature_get_payload(d).dropbug.jump_at_player = 0;
        w.creature_get_payload(d).dropbug.jump_at_chunk = 0;
        w.dropbug_data_set_jump_at_pos(creature::dropbug_data(w, d), vec2{0.f, 0.f});
        w.creature_get_payload(d).dropbug.attempt_bite = 0.f;
        w.creature_get_payload(d).dropbug.jumping = 0;
        w.creature_get_payload(d).dropbug.from_ceiling_jump = 0;
        w.creature_get_payload(d).dropbug.drop_anticipation = 0.f;
        w.creature_get_payload(d).dropbug.grab_on_next_attack = 0;
        w.creature_get_payload(d).dropbug.release_grabbed_counter = 0;
        w.dropbug_data_set_lured_to_drop_counter(creature::dropbug_data(w, d), 0);
        w.creature_get_payload(d).dropbug.after_drop_attack_delay = 0;
        w.dropbug_data_set_carry_object_mass(creature::dropbug_data(w, d), 0.f);
        w.dropbug_data_set_walk_backwards_dist(creature::dropbug_data(w, d), 0.f);
        w.dropbug_data_set_stuck_shake(creature::dropbug_data(w, d), 0.f);
        w.creature_get_payload(d).dropbug.in_ceiling_mode = 0.f;
        w.creature_get_payload(d).dropbug.voice = 0;
        w.creature_get_grasp_player(d)[0] = 0;
        w.creature_get_grasp_chunk(d)[0] = 0;
        w.creature_set_behavior(d, uint8_t(behavior::idle));
        w.creature_set_current_utility(d, 0.f);
        w.dropbug_data_set_noise_reaction_delay(creature::dropbug_data(w, d), 0);
        w.dropbug_data_set_target_player(creature::dropbug_data(w, d), 0);
        w.dropbug_data_set_attack_counter(creature::dropbug_data(w, d), 0);
        w.dropbug_data_set_idle_pos_counter(creature::dropbug_data(w, d), 0);
        w.dropbug_data_set_walk_with_bug(creature::dropbug_data(w, d), std::nullopt);
        w.creature_set_seen_player(d, 0);
        w.creature_set_sees_player(d, 0);
        w.creature_set_ticks_since_seen(d, 0);
        w.creature_set_last_seen_pos(d, at);
        w.dropbug_data_set_utilities(creature::dropbug_data(w, d), utilities{});
        w.dropbug_data_set_stuck(creature::dropbug_data(w, d), stuck_tracker{});
        // CeilingSitModule.NewRoom
        ceiling_module c;
        auto& r = w.creature_get_random(d);
        c.ceiling_pos = random_tile(r, view);
        c.stay_away_from_pos = random_tile(r, view);
        c.random_attractor = random_tile(r, view);
        c.room_size = std::max(view.width, view.height);
        w.dropbug_data_set_ceiling(creature::dropbug_data(w, d), c);

        // DropBug.PlaceInRoom
        ivec2 tile = room_view::get_tile_position(at);
        if (!valid_ceiling_spot(state, view, tile)) {
            std::optional<ivec2> spot;
            if (relocate) {
                spot = best_ceiling_spot(state, d, view);
            } else {
                for (ivec2 t = tile; view.inside(t.x, t.y) && !solid(view, t) && !spot; ++t.y)
                    if (valid_ceiling_spot(state, view, t))
                        spot = t;
            }
            if (spot) {
                tile = *spot;
                at = room_view::middle_of_tile(tile);
                for (uint32_t i = 0; i < 3; ++i)
                    physics::hard_set_position(state, chunk(state, d, i), at + vec2{0.f, float(i)});
            }
        }
        w.creature_set_destination(d, tile);
        w.dropbug_data_set_temp_idle_pos(creature::dropbug_data(w, d), tile);
        if (valid_ceiling_spot(state, view, tile)) {
            w.creature_get_payload(d).dropbug.in_ceiling_mode = 1.f;
            auto& cm = w.dropbug_data_get_ceiling(creature::dropbug_data(w, d));
            cm.ceiling_pos = tile;
            cm.win_streak = 1000;
            w.dropbug_data_get_utilities(creature::dropbug_data(w, d)).ceiling = 1.f;
            w.creature_get_payload(d).dropbug.footing_counter = 100;
            w.creature_set_behavior(d, uint8_t(behavior::sit_in_ceiling));
        }
        dropbug_graphics::reset(state, d);
        return d;
    }

    void destroy(sys::game_state& state, dcon::creature_id d) {
        lose_all_grasps(state, d);
        creature::erase(state, d);
    }

    bool collides(sys::game_state const& state, dcon::creature_id d, dcon::player_id p) {
        // what it holds is kept out of it by CarryObject
        return !state.world.player_get_in_shortcut(p) && grasped(state, d) != p;
    }

    void collide_player(sys::game_state& state, dcon::creature_id d, uint32_t i, dcon::player_id p, uint32_t j) {
        if (consious(state, d))
            collide_with_player(state, d, i, p, j);
    }

    void collide_kin(sys::game_state& state, dcon::creature_id d, uint32_t i, dcon::creature_id other, uint32_t j) {
        // DropBug.Collide of the upper one: it climbs over
        auto& w = state.world;
        auto const a = chunk(state, d, i), b = chunk(state, other, j);
        bool const above = w.body_chunk_get_pos(a).y > w.body_chunk_get_pos(b).y;
        w.body_chunk_get_vel(above ? a : b).y += 2.f;
        w.body_chunk_get_vel(above ? b : a).y -= 2.f;
    }

    void touched_kin(sys::game_state& state, dcon::creature_id d, dcon::creature_id other) {
        // dropbugs follow the dominant ones
        collide_with_kin(state, d, other);
        collide_with_kin(state, other, d);
    }

    void violence(sys::game_state& state, dcon::creature_id d, uint32_t k, vec2 momentum, float damage, float stun_bonus) {
        auto& w = state.world;
        auto& rng = w.creature_get_random(d);
        // DropBug.Violence: the head is the soft spot
        damage *= k == 0 ? mathf::lerp(0.975f, 1.25f, rng.value()) : mathf::lerp(0.9f, 1.1f, rng.value());
        if (!w.creature_get_dead(d) && (damage > 0.1f || stun_bonus > 20.f) && rng.value() < custom::lerp_map(damage, 0.3f, 1.f, 0.4f, 0.95f))
            play_voice(state, d);
        // Creature.Violence
        auto const hit = chunk(state, d, k);
        w.body_chunk_get_vel(hit) += rw::clamp_magnitude(momentum / w.body_chunk_get_mass(hit), 10.f);
        float const dmg = damage / base_damage_resistance;
        float st = (damage * 30.f + stun_bonus) / base_stun_resistance;
        float& health = w.creature_get_health(d);
        st *= 1.5f + mathf::inverse_lerp(0.5f, 0.f, health) * rng.value();
        stun(state, d, int32_t(st));
        health -= dmg;
        // quickDeath
        if (rng.value() < -health || health < -1.f || (health < 0.f && rng.value() < 0.33f))
            die(state, d);
        if (rng.value() < 0.5f && health < 0.f)
            die(state, d);
    }

    void flying_weapon(sys::game_state& state, dcon::creature_id d, vec2 at, vec2 v, vec2 thrown_pos) {
        auto& w = state.world;
        auto const room = room_of(state, d);
        {
            if (!consious(state, d))
                return;
            auto const view = world::get_room_view(state, room);
            vec2 const head = pos(state, d, 0);
            if (!is_sitting_in_ceiling(state, d, view) || std::fabs(at.x - thrown_pos.x) >= 200.f || !custom::dist_less(at + rw::normalized(v) * 100.f, head, 120.f))
                return;
            float const off = std::fabs(custom::distance_to_line(head, at, at + v));
            if (off < 15.f && visual_contact(state, d, view, at)) {
                dislodge(state, d, view);
                return;
            }
            if (at.y < head.y - 10.f && off < 35.f)
                w.dropbug_data_set_lured_to_drop_counter(creature::dropbug_data(w, d), 30);
        }
    }

    void release_player(sys::game_state& state, dcon::creature_id d, dcon::player_id p) {
        if (grasped(state, d) == p)
            lose_all_grasps(state, d);
    }
}
