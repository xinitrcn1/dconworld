#include <cmath>
#include <limits>
#include <optional>

#include "tube_worm.hpp"
#include "dens.hpp"
#include "migrate.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "rooms.hpp"
#include "rot.hpp"
#include "tube_worm_graphics.hpp"

namespace tube_worm {
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // TubeWorm.maxTotalRope
        constexpr float max_total_rope = 200.f;
        // CreatureTemplate: baseDamageResistance, baseStunResistance, instantDeathDamageLimit
        constexpr float base_damage_resistance = 0.3f;
        constexpr float base_stun_resistance = 0.3f;
        constexpr float instant_death_damage_limit = 1.2f;
        // its tongue's ray traces (Tongue.Shoot's auto aims)
        constexpr float aim_reach = 230.f;

        creature::tube_worm_payload& pl(sys::game_state& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).tube_worm;
        }
        creature::tube_worm_payload const& pl(sys::game_state const& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).tube_worm;
        }
        dcon::tube_worm_data_id data(sys::game_state const& state, dcon::creature_id c) {
            return creature::tube_worm_data(state.world, c);
        }

        //
        // SharedPhysics' tile ray traces
        //
        // the tiles of the line from `a` to `b` in order (RayTracedTilesArray's
        // walk), until `f` returns true; whether it did
        template<typename F>
        bool walk_tiles(vec2 a, vec2 b, F&& f) {
            float const ax = a.x / 20.f, ay = a.y / 20.f, bx = b.x / 20.f, by = b.y / 20.f;
            float const dx = std::fabs(bx - ax), dy = std::fabs(by - ay);
            int32_t x = int32_t(std::floor(ax));
            int32_t y = int32_t(std::floor(ay));
            float const dt_dx = 1.f / dx, dt_dy = 1.f / dy;
            int32_t n = 1;
            int32_t x_inc, y_inc;
            float t_next_h, t_next_v;
            if (dx == 0.f) {
                x_inc = 0;
                t_next_h = dt_dx;
            } else if (bx > ax) {
                x_inc = 1;
                n += int32_t(std::floor(bx)) - x;
                t_next_h = (std::floor(ax) + 1.f - ax) * dt_dx;
            } else {
                x_inc = -1;
                n += x - int32_t(std::floor(bx));
                t_next_h = (ax - std::floor(ax)) * dt_dx;
            }
            if (dy == 0.f) {
                y_inc = 0;
                t_next_v = dt_dy;
            } else if (by > ay) {
                y_inc = 1;
                n += int32_t(std::floor(by)) - y;
                t_next_v = (std::floor(ay) + 1.f - ay) * dt_dy;
            } else {
                y_inc = -1;
                n += y - int32_t(std::floor(by));
                t_next_v = (ay - std::floor(ay)) * dt_dy;
            }
            n = std::min(n, 4096);
            for (; n > 0; --n) {
                if (f(ivec2{x, y}))
                    return true;
                if (t_next_v < t_next_h) {
                    y += y_inc;
                    t_next_v += dt_dy;
                } else {
                    x += x_inc;
                    t_next_h += dt_dx;
                }
            }
            return false;
        }
        // RayTraceTilesForTerrainReturnFirstSolid
        std::optional<ivec2> first_solid(room_view const& room, vec2 a, vec2 b) {
            ivec2 hit{0, 0};
            if (walk_tiles(a, b, [&](ivec2 t) { return room.get_tile(t).solid() ? (hit = t, true) : false; }))
                return hit;
            return std::nullopt;
        }
        // RayTraceTilesForTerrain: no solid tile in the way
        bool clear_of_terrain(room_view const& room, vec2 a, vec2 b) {
            return !first_solid(room, a, b).has_value();
        }
        // RayTracedTilesArray up to the first solid tile: the one before it
        std::optional<ivec2> tile_before_solid(room_view const& room, vec2 a, vec2 b) {
            std::optional<ivec2> last;
            std::optional<ivec2> found;
            walk_tiles(a, b, [&](ivec2 t) {
                if (last && room.get_tile(t).solid()) {
                    found = last;
                    return true;
                }
                last = t;
                return false;
            });
            return found;
        }

        // Custom.VerticalCrossPoint / HorizontalCrossPoint
        vec2 vertical_cross_point(vec2 a, vec2 b, float x) {
            if (a.x == b.x)
                return vec2{x, std::numeric_limits<float>::quiet_NaN()};
            float const k = (a.y - b.y) / (a.x - b.x);
            return vec2{x, a.y - a.x * k + k * x};
        }
        vec2 horizontal_cross_point(vec2 a, vec2 b, float y) {
            if (a.y == b.y)
                return vec2{std::numeric_limits<float>::quiet_NaN(), y};
            if (a.x == b.x)
                return vec2{a.x, y};
            float const k = (a.y - b.y) / (a.x - b.x);
            return vec2{(y - (a.y - a.x * k)) / k, y};
        }
        // Custom.RectCollision's lower left corner: where the segment from
        // `last` to `pos` enters the rectangle
        vec2 rect_collision(vec2 pos, vec2 last, float left, float bottom, float right, float top) {
            vec2 const v1 = vertical_cross_point(last, pos, left);
            vec2 const v2 = vertical_cross_point(last, pos, right);
            vec2 v3 = horizontal_cross_point(last, pos, bottom);
            vec2 v4 = horizontal_cross_point(last, pos, top);
            if (last.x == pos.x) {
                v3 = vec2{last.x, bottom};
                v4 = vec2{last.x, top};
            }
            bool const inside = last.x > left && last.x < right && last.y > bottom && last.y < top;
            float dst = inside ? std::numeric_limits<float>::max() : rw::distance(last, pos);
            vec2 out = pos;
            auto const take = [&](vec2 v) {
                out = v;
                dst = rw::distance(last, v);
            };
            if (v1.y >= bottom && v1.y <= top && custom::dist_less(last, v1, dst))
                take(v1);
            if (v2.y >= bottom && v2.y <= top && custom::dist_less(last, v2, dst))
                take(v2);
            if (v3.x >= left && v3.x <= right && custom::dist_less(last, v3, dst))
                take(v3);
            if (v4.x >= left && v4.x <= right && custom::dist_less(last, v4, dst))
                take(v4);
            return out;
        }
        // SharedPhysics.ExactTerrainRayTracePos
        std::optional<vec2> exact_terrain_ray_trace_pos(room_view const& room, vec2 a, vec2 b) {
            auto const t = first_solid(room, a, b);
            if (!t)
                return std::nullopt;
            vec2 const m = room_view::middle_of_tile(*t);
            return rect_collision(b, a, m.x - 10.f, m.y - 10.f, m.x + 10.f, m.y + 10.f);
        }

        //
        // Update's working set
        //
        struct ctx {
            sys::game_state& state;
            dcon::creature_id c;
            room_view const& room;
            creature::tube_worm_payload& x;
            dcon::tube_worm_data_id d;
            tongue_pair& tp;
            rw::rng rng;
            dcon::body_chunk_id ch[2];
            // the tongues' baseChunk (the slugcat's when it holds the worm)
            dcon::body_chunk_id base[2];
            // the slugcat holding it (grabbedBy[0].grabber is Player)
            dcon::player_id holder;
            // the pathfinder's field to the destination, when asked for
            std::optional<pathing::field_id> field;

            // (keyed by the constructing call site too: several ctxs of one
            // worm in one tick roll apart)
            ctx(sys::game_state& s, dcon::creature_id cc, room_view const& r, std::source_location const l = std::source_location::current())
                : state(s), c(cc), room(r), x(pl(s, cc)), d(data(s, cc)), tp(s.world.tube_worm_data_get_tongues(d)), rng(rw::random(s.seed, s.tick, uint32_t(cc.value), l)) {
                ch[0] = chunk(s, cc, 0);
                ch[1] = chunk(s, cc, 1);
                base[0] = ch[0];
                base[1] = ch[1];
            }
            auto& w() { return state.world; }
            tongue& t(uint32_t i) { return tp.t[i]; }
            vec2& pos(uint32_t i) { return w().body_chunk_get_pos(ch[i]); }
            vec2& vel(uint32_t i) { return w().body_chunk_get_vel(ch[i]); }
            vec2& base_pos(uint32_t i) { return w().body_chunk_get_pos(base[i]); }
            vec2& base_vel(uint32_t i) { return w().body_chunk_get_vel(base[i]); }
            ivec2 contact(uint32_t i) { return w().body_chunk_get_contact_point(ch[i]); }
            // abstractCreature.pos.Tile
            ivec2 tile() { return room_view::get_tile_position(pos(0)); }
            bool accessible(ivec2 t) { return room.inside(t.x, t.y) && pathing::accessible(room, t, pathing::tube_worm); }
            pathing::field_id path() {
                if (!field)
                    field = pathing::field_to(state, room.id, w().creature_get_destination(c), pathing::tube_worm);
                return *field;
            }
            // PathFinder.PathingCellAtWorldCoordinate(t).costToGoal (none for
            // a tile off its map: the fallback cell)
            uint16_t cost(ivec2 t) { return accessible(t) ? pathing::distance(state, path(), t) : pathing::unreachable; }
            void sound(sound::sound_id id, vec2 at) { state.play_sound(id, room.id, at); }
        };

        // ComparePathingCells: `a` is the better cell (closer to the goal)
        bool better(ctx& x, std::optional<ivec2> a, std::optional<ivec2> b) {
            uint16_t const ca = a ? x.cost(*a) : pathing::unreachable;
            if (ca == pathing::unreachable)
                return false;
            uint16_t const cb = b ? x.cost(*b) : pathing::unreachable;
            return cb == pathing::unreachable || ca < cb;
        }

        //
        // The rope between the tongues
        //
        float total_rope(tongue_pair const& tp) {
            if (tp.t[0].is_attached() && tp.t[1].is_attached())
                return mathf::min(max_total_rope, rw::distance(tp.t[0].pos, tp.t[1].pos));
            return max_total_rope;
        }
        // WeightedRopeRequest
        float weighted_request(tongue_pair const& tp, float on_rope_pos, uint32_t i) {
            return mathf::min(tp.t[i].requested_rope_length, (i == 0 ? on_rope_pos : 1.f - on_rope_pos) * total_rope(tp));
        }
        // RequestRope
        float request_rope(tongue_pair const& tp, float on_rope_pos, uint32_t i) {
            float const a = weighted_request(tp, on_rope_pos, i);
            float const b = weighted_request(tp, on_rope_pos, 1 - i);
            if (a + b < total_rope(tp))
                return a;
            return a + b > 0.f ? total_rope(tp) * (a / (a + b)) : 0.f;
        }
        float rope_stretch(tongue_pair const& tp, float on_rope_pos) {
            float n = mathf::lerp(max_total_rope, request_rope(tp, on_rope_pos, 0) + request_rope(tp, on_rope_pos, 1), 0.5f)
                / (tp.t[0].rope.total_length + tp.t[1].rope.total_length + 80.f);
            n = mathf::pow(n, n < 1.f ? 1.6f : 0.4f);
            if (tp.t[0].mode == tongue_mode::attached_to_terrain && tp.t[1].mode == tongue_mode::attached_to_terrain)
                n = mathf::lerp(n, 1.f, 0.5f);
            return n;
        }

        // Rope.Reset(pos)
        void reset_rope(world::rope_state& r, vec2 at) {
            r.bend_count = 0;
            r.a = r.last_a = r.b = r.last_b = at;
            r.total_length = 0.f;
            r.thickness = 1.f;
        }

        //
        // TubeWorm.Tongue
        //
        void release(ctx& x, uint32_t i) {
            auto& t = x.t(i);
            if (t.mode == tongue_mode::attached_to_object && t.attached)
                x.sound(sound::sound_id::tube_worm_detatch_tongue_creature, t.pos);
            else if (t.mode == tongue_mode::attached_to_terrain)
                x.sound(sound::sound_id::tube_worm_detach_tongue_terrain, t.pos);
            t.mode = tongue_mode::retracting;
            t.attached = dcon::creature_id{};
        }

        void attach(ctx& x, uint32_t i) {
            auto& t = x.t(i);
            t.vel = vec2{0.f, 0.f};
            t.elastic = 1.f;
            t.requested_rope_length = rw::distance(x.base_pos(i), t.pos);
        }
        void attach_to_terrain(ctx& x, uint32_t i, vec2 at) {
            auto& t = x.t(i);
            t.terrain_stuck_pos = at;
            t.mode = tongue_mode::attached_to_terrain;
            t.pos = at;
            attach(x, i);
            x.sound(sound::sound_id::tube_worm_tongue_hit_terrain, t.pos);
        }
        void attach_to_chunk(ctx& x, uint32_t i, dcon::creature_id who, uint32_t k) {
            auto& t = x.t(i);
            t.attached = who;
            t.attached_chunk = uint8_t(k);
            t.pos = x.w().body_chunk_get_pos(creature::chunk(x.state, who, k));
            t.mode = tongue_mode::attached_to_object;
            attach(x, i);
            x.sound(sound::sound_id::tube_worm_tongue_hit_creature, t.pos);
        }

        // ProperAutoAim: held by a slugcat, the direction (within 35
        // degrees either way) whose ray hits the terrain closest to the
        // rope's ideal length (or, over a floor, one short of it), towards
        // where it pushes, with open space below; where it hits is
        // playerCheatAttachPos
        vec2 proper_auto_aim(ctx& x, uint32_t i, vec2 original) {
            auto const& in = x.w().player_get_input(x.holder)[0];
            vec2 const bp = x.base_pos(i);
            vec2 const ahead = bp + x.base_vel(i) * 3.f;
            float ideal = x.t(i).ideal_rope_length;
            float const deg = custom::vec_to_deg(original);
            int32_t const push = original.y < 0.9f ? in.x : 0;
            bool const up = in.y > 0;
            bool over_floor = false;
            if (!up) {
                ivec2 tile = room_view::get_tile_position(ahead);
                for (int k = 0; k < 10; ++k) {
                    --tile.y;
                    if (x.room.get_tile(tile).solid()) {
                        ideal = mathf::max(40.f, rw::distance(ahead, room_view::middle_of_tile(tile)) - 40.f);
                        over_floor = true;
                        break;
                    }
                }
            }
            vec2 result = original;
            float best = std::numeric_limits<float>::max();
            auto& cheat = x.w().tube_worm_data_get_player_cheat_attach_pos(x.d);
            for (float a = 0.f; a < 35.f; a += 2.5f) {
                for (float side = -1.f; side <= 1.f; side += 2.f) {
                    float const ang = deg + a * side;
                    auto const hit = exact_terrain_ray_trace_pos(x.room, bp, bp + custom::deg_to_vec(ang) * aim_reach);
                    if (!hit)
                        continue;
                    float score = a * 1.5f;
                    if (!up) {
                        score += std::fabs(ideal - rw::distance(ahead, *hit));
                        if (push != 0)
                            score += std::fabs(float(push) * 90.f - ang) * 0.9f;
                        if (over_floor)
                            for (int j = -1; j < 2; ++j)
                                if (!x.room.visual_contact(*hit, *hit - vec2{40.f * float(j), rw::distance(bp, *hit) + 20.f})) {
                                    score += 1000.f;
                                    break;
                                }
                    }
                    if (score < best) {
                        best = score;
                        result = custom::deg_to_vec(ang);
                        cheat = *hit + custom::dir_vec(*hit, bp) * 2.f;
                    }
                }
            }
            return result;
        }

        // CheapAutoAim: the first direction within 25 degrees whose ray hits
        // the terrain, if the original one doesn't
        vec2 cheap_auto_aim(ctx& x, uint32_t i, vec2 original) {
            vec2 const bp = x.base_pos(i);
            if (!clear_of_terrain(x.room, bp, bp + original * aim_reach))
                return original;
            float const deg = custom::vec_to_deg(original);
            for (float a = 5.f; a < 30.f; a += 5.f)
                for (float side = -1.f; side <= 1.f; side += 2.f)
                    if (!clear_of_terrain(x.room, bp, bp + custom::deg_to_vec(deg + a * side) * aim_reach))
                        return custom::deg_to_vec(deg + a * side);
            return original;
        }

        void shoot(ctx& x, uint32_t i, vec2 dir) {
            auto& t = x.t(i);
            x.w().tube_worm_data_set_player_cheat_attach_pos(x.d, std::nullopt);
            if (t.is_attached()) {
                release(x, i);
                return;
            }
            if (t.mode != tongue_mode::retracted)
                return;
            t.mode = tongue_mode::shooting_out;
            x.sound(sound::sound_id::tube_worm_shoot_tongue, x.base_pos(i));
            dir = x.holder ? proper_auto_aim(x, i, dir) : cheap_auto_aim(x, i, dir);
            t.pos = x.base_pos(i) + dir * 5.f;
            t.vel = dir * 70.f;
            t.elastic = 1.f;
            t.requested_rope_length = 140.f;
            t.returning = 0;
        }

        // Tongue.Elasticity: the rope longer than asked for pulls its two
        // ends together, by their weights
        void elasticity(ctx& x, uint32_t i) {
            auto& w = x.w();
            auto& t = x.t(i);
            float pull = 0.f;
            dcon::body_chunk_id held;
            if (t.mode == tongue_mode::attached_to_terrain) {
                pull = 1.f;
            } else if (t.mode == tongue_mode::attached_to_object) {
                held = creature::chunk(x.state, t.attached, t.attached_chunk);
                float const m = w.body_chunk_get_mass(held);
                pull = m / (m + w.body_chunk_get_mass(x.base[i]));
            }
            vec2 a = custom::dir_vec(x.base_pos(i), rot::rope_a_connect(t.rope));
            float const total = t.rope.total_length;
            float slack = 0.7f;
            if (x.t(0).is_attached() && x.t(1).is_attached())
                slack = custom::lerp_map(std::fabs(0.5f - x.x.on_rope_pos), 0.5f, 0.4f, 1.1f, 0.7f);
            float const want = request_rope(x.tp, x.x.on_rope_pos, i) * mathf::lerp(slack, 1.f, t.elastic);
            float const k = mathf::lerp(0.85f, 0.25f, t.elastic);
            if (total <= want)
                return;
            float const over = (total - want) * k;
            x.base_vel(i) += a * over * pull;
            x.base_pos(i) += a * over * pull * mathf::lerp(1.f, t.mode == tongue_mode::attached_to_terrain ? 1.f : 0.5f, t.elastic);
            a = custom::dir_vec(t.pos, rot::rope_b_connect(t.rope));
            if (t.free()) {
                t.vel += a * over * (1.f - pull);
                t.pos += a * over * (1.f - pull) * mathf::lerp(1.f, 0.5f, t.elastic);
                return;
            }
            if (t.mode == tongue_mode::attached_to_object) {
                w.body_chunk_get_vel(held) += a * over * (1.f - pull);
                w.body_chunk_get_pos(held) += a * over * (1.f - pull) * mathf::lerp(1.f, 0.5f, t.elastic);
            }
        }

        // Tongue.Update
        void tongue_update(ctx& x, uint32_t i) {
            auto& w = x.w();
            auto& t = x.t(i);
            t.last_pos = t.pos;
            t.pos += t.vel;
            switch (t.mode) {
            case tongue_mode::retracted:
                t.requested_rope_length = 0.f;
                t.pos = x.pos(0);
                t.vel = x.vel(0);
                t.rope.bend_count = 0;
                break;
            case tongue_mode::shooting_out: {
                t.requested_rope_length = mathf::max(0.f, t.requested_rope_length - 4.f);
                bool hit = false;
                if (!custom::dist_less(x.base_pos(i), t.pos, 60.f)) {
                    // TraceProjectileAgainstBodyChunks (not the base chunk's owner)
                    dcon::creature_id who;
                    creature::hit_part part;
                    auto const owner = x.holder ? player::creature_of(x.state, x.holder) : x.c;
                    if (creature::trace(x.state, x.room.id, t.pos, t.pos + t.vel, 1.f, owner, who, part) && part.chunk >= 0 && who != x.c) {
                        attach_to_chunk(x, i, who, uint32_t(part.chunk));
                        hit = true;
                    }
                }
                if (hit)
                    break;
                auto const cheat = w.tube_worm_data_get_player_cheat_attach_pos(x.d);
                if (cheat && custom::dist_less(t.pos, *cheat, 60.f)) {
                    attach_to_terrain(x, i, *cheat);
                    w.tube_worm_data_set_player_cheat_attach_pos(x.d, std::nullopt);
                    break;
                }
                if (auto const s = first_solid(x.room, t.last_pos, t.pos)) {
                    vec2 const m = room_view::middle_of_tile(*s);
                    attach_to_terrain(x, i, rect_collision(t.pos, t.last_pos, m.x - 11.f, m.y - 11.f, m.x + 11.f, m.y + 11.f));
                    break;
                }
                t.vel.y -= 0.9f * mathf::inverse_lerp(0.8f, 0.f, t.elastic);
                if (t.returning) {
                    // a pole on the way back catches it
                    walk_tiles(t.last_pos, t.pos, [&](ivec2 k) {
                        auto const tile = x.room.get_tile(k);
                        vec2 const m = room_view::middle_of_tile(k);
                        if (tile.horizontal_beam()) {
                            attach_to_terrain(x, i, vec2{mathf::clamp(horizontal_cross_point(t.last_pos, t.pos, m.y).x, m.x - 10.f, m.x + 10.f), m.y});
                            return true;
                        }
                        if (tile.vertical_beam()) {
                            attach_to_terrain(x, i, vec2{m.x, mathf::clamp(vertical_cross_point(t.last_pos, t.pos, m.x).y, m.y - 10.f, m.y + 10.f)});
                            return true;
                        }
                        return false;
                    });
                    if (custom::dist_less(x.base_pos(i), t.pos, 40.f))
                        t.mode = tongue_mode::retracted;
                } else if (rw::dot(custom::dir_vec(x.base_pos(i), t.pos), rw::normalized(t.vel)) < 0.f) {
                    t.returning = 1;
                }
                break;
            }
            case tongue_mode::retracting:
                t.mode = tongue_mode::retracted;
                break;
            case tongue_mode::attached_to_terrain:
                // (Remix's zap coils and the no spear stick zones aren't ported)
                t.pos = t.terrain_stuck_pos;
                t.vel = vec2{0.f, 0.f};
                break;
            case tongue_mode::attached_to_object:
                if (t.attached && creature::here(x.state, t.attached, x.room.id) && t.attached_chunk < creature::chunk_count(x.state, t.attached)) {
                    auto const k = creature::chunk(x.state, t.attached, t.attached_chunk);
                    t.pos = w.body_chunk_get_pos(k);
                    t.vel = w.body_chunk_get_vel(k);
                } else {
                    t.attached = dcon::creature_id{};
                    t.mode = tongue_mode::retracting;
                }
                break;
            }
            rot::update_rope(t.rope, x.room, x.base_pos(i), t.pos);
            if (t.mode != tongue_mode::retracted)
                elasticity(x, i);
            if (t.is_attached()) {
                t.elastic = mathf::max(0.f, t.elastic - 0.05f);
                if (t.elastic <= 0.f)
                    t.rope_extend_speed = mathf::min(t.rope_extend_speed + 0.025f, 1.f);
                if (t.requested_rope_length < t.ideal_rope_length)
                    t.requested_rope_length = mathf::min(t.requested_rope_length + t.rope_extend_speed * 2.f, t.ideal_rope_length);
                else if (t.requested_rope_length > t.ideal_rope_length)
                    t.requested_rope_length = mathf::max(t.requested_rope_length - (1.f - t.elastic) * 2.f, t.ideal_rope_length);
            } else {
                t.rope_extend_speed = 0.f;
            }
        }

        //
        // TubeWorm
        //
        void walk_along_rope(ctx& x) {
            auto& p = x.x;
            float const total = total_rope(x.tp);
            float const speed = p.crawl_speed / total;
            float const before = p.on_rope_pos;
            if (p.on_rope_pos > p.goal_on_rope_pos)
                p.on_rope_pos = mathf::max(p.goal_on_rope_pos, p.on_rope_pos - speed);
            else
                p.on_rope_pos = mathf::min(p.goal_on_rope_pos, p.on_rope_pos + speed);
            p.walk_cycle += (before - p.on_rope_pos) * total * 0.021f;
        }

        void sleep(ctx& x) {
            auto& p = x.x;
            if (--p.sleep_counter < 1)
                p.sleeping = 0;
            for (uint32_t i = 0; i < 2; ++i)
                if (x.contact(i) != ivec2{0, 0})
                    p.sleeping = 0;
            if (x.rng.value() < 0.0020833334f)
                p.goal_on_rope_pos = x.rng.value();
            walk_along_rope(x);
        }

        // TubeWormAI.Update: a random destination now and then, or once close
        // to the last one; a trip to another room takes its way out
        void ai_update(ctx& x) {
            auto& w = x.w();
            migrate::idle_roll(x.state, x.c, x.room);
            if (auto const t = migrate::travel_tile(x.state, x.c, x.room)) {
                if (w.creature_get_destination(x.c) != *t) {
                    w.creature_set_destination(x.c, *t);
                    x.field.reset();
                }
                return;
            }
            int32_t& counter = w.tube_worm_data_get_dest_counter(x.d);
            --counter;
            if (counter >= 1 && custom::manhattan_distance(x.tile(), w.creature_get_destination(x.c)) >= 6)
                return;
            // RandomDest: a tile it can get to (CoordinateReachableAndGetbackable)
            ivec2 const dest{x.rng.range(0, x.room.width), x.rng.range(0, x.room.height)};
            if (!x.accessible(dest))
                return;
            auto const f = pathing::field_to(x.state, x.room.id, dest, pathing::tube_worm);
            ivec2 const here = x.tile();
            bool reachable = false;
            for (auto const dv : custom::eight_directions_and_zero)
                reachable = reachable || (x.accessible(here + dv) && pathing::distance(x.state, f, here + dv) != pathing::unreachable);
            if (!reachable)
                return;
            w.creature_set_destination(x.c, dest);
            x.field = f;
            counter = x.rng.range(400, 1200);
        }

        // StandardPather.FollowPath from `from`: the tile to step to
        std::optional<ivec2> follow_path(ctx& x, ivec2 from) {
            if (!x.accessible(from))
                return std::nullopt;
            return pathing::next_step(x.state, x.path(), from);
        }

        void find_inaccessible_grappling_pos(ctx& x) {
            auto& w = x.w();
            uint32_t const s = x.x.step;
            auto const found = tile_before_solid(x.room, x.pos(s), x.pos(s) + custom::rnv(x.rng) * 220.f);
            if (!found || x.accessible(*found) || *found == x.tile())
                return;
            vec2 const a = room_view::middle_of_tile(*found);
            auto const last = w.tube_worm_data_get_last_accessible(x.d);
            bool good = false;
            float const turn = 45.f * x.rng.value();
            for (float ang = 0.f; ang < 360.f && !good; ang += 45.f) {
                // from the spot, a ray past its first 6 tiles to the terrain:
                // the tile before it closer to the goal than where it is
                int32_t k = 0;
                std::optional<ivec2> prev;
                walk_tiles(a, a + custom::deg_to_vec(ang + turn) * 220.f, [&](ivec2 t) {
                    bool stop = false;
                    if (k > 6 && x.room.get_tile(t).solid()) {
                        good = better(x, prev, last);
                        stop = true;
                    }
                    prev = t;
                    ++k;
                    return stop;
                });
            }
            if (good)
                w.tube_worm_data_set_grapple_to_pos(x.d, *found);
        }

        void find_accessible_grappling_pos(ctx& x) {
            auto& w = x.w();
            vec2 const from = room_view::middle_of_tile(*w.tube_worm_data_get_grapple_to_pos(x.d));
            auto const found = tile_before_solid(x.room, from, from + custom::rnv(x.rng) * 220.f);
            if (!found || !x.accessible(*found) || *found == x.tile())
                return;
            auto const next = w.tube_worm_data_get_next_grapple_pos(x.d);
            if (!next || better(x, found, next))
                w.tube_worm_data_set_next_grapple_pos(x.d, found);
        }

        // its end `step` thrown towards `p`, the other end the other way;
        // true when it lines up well enough to shoot the tongue (`aim` at
        // the start, lower with the tries)
        bool swing_end_at(ctx& x, vec2 p, float aim) {
            uint32_t const s = x.x.step;
            vec2 v = custom::dir_vec(x.pos(s), p);
            v = rw::slerp(v, custom::rnv(x.rng), mathf::inverse_lerp(2.f, 8.f, float(x.x.shoot_tries)));
            float const f = custom::lerp_map(float(x.x.tried_move), 0.f, 40.f, 0.6f, 1.7f);
            x.vel(s) += v * f;
            x.vel(1 - s) -= v * f;
            if (rw::dot(rw::normalized(x.pos(s) - x.pos(1 - s)), v) <= custom::lerp_map(float(x.x.tried_move), 0.f, 100.f, aim, -1.f))
                return false;
            shoot(x, s, v);
            return true;
        }

        void grapple_to_position(ctx& x) {
            auto& w = x.w();
            auto& p = x.x;
            if (!x.accessible(*w.tube_worm_data_get_grapple_to_pos(x.d)))
                find_accessible_grappling_pos(x);
            uint32_t const s = p.step;
            if (x.t(s).is_attached()) {
                p.goal_on_rope_pos = float(s);
                walk_along_rope(x);
                if (p.on_rope_pos == float(s)) {
                    release(x, 1 - s);
                    p.step = uint8_t(1 - s);
                    w.tube_worm_data_set_grapple_to_pos(x.d, w.tube_worm_data_get_next_grapple_pos(x.d));
                    w.tube_worm_data_set_next_grapple_pos(x.d, std::nullopt);
                    p.tried_move = 0;
                    p.grappling_counter = 0;
                    p.shoot_tries = 0;
                }
                return;
            }
            vec2 const to = room_view::middle_of_tile(*w.tube_worm_data_get_grapple_to_pos(x.d));
            if (custom::dist_less(x.pos(s), to, 220.f) && x.room.visual_contact(x.pos(s), to)) {
                if (swing_end_at(x, to, 1.5f))
                    ++p.shoot_tries;
                return;
            }
            w.tube_worm_data_set_grapple_to_pos(x.d, std::nullopt);
        }

        void hold_on_to_object(ctx& x) {
            bool both = true;
            for (uint32_t i = 0; i < 2; ++i) {
                if (x.rng.value() < 0.00027777778f)
                    release(x, i);
                if (!x.t(i).is_attached())
                    both = false;
                if (x.t(i).mode == tongue_mode::retracted && x.contact(i) != ivec2{0, 0})
                    shoot(x, i, rw::to_vec2(x.contact(i)));
            }
            if (both) {
                if (x.rng.value() < 0.0038461538f)
                    x.x.goal_on_rope_pos = x.rng.value();
                walk_along_rope(x);
            }
        }

        void flop_along(ctx& x) {
            auto& w = x.w();
            auto& p = x.x;
            if (p.tried_move > 200) {
                for (uint32_t i = 0; i < 2; ++i) {
                    release(x, i);
                    x.vel(i) += custom::rnv(x.rng) * x.rng.value() * 12.f;
                }
                p.tried_move = 0;
            }
            uint32_t const s = p.step;
            uint32_t const o = 1 - s;
            if (!x.t(o).is_attached()) {
                if (x.contact(o).y < 0)
                    shoot(x, o, vec2{0.f, -1.f});
                return;
            }
            if (x.t(s).is_attached()) {
                if (custom::dist_less(x.pos(s), x.t(s).pos, 100.f)) {
                    release(x, o);
                    p.step = uint8_t(o);
                } else {
                    w.tube_worm_data_set_grapple_to_pos(x.d, room_view::get_tile_position(x.t(s).pos + custom::dir_vec(x.t(s).pos, x.pos(s)) * 5.f));
                }
                p.shoot_tries = 0;
                return;
            }
            if (s == 1)
                p.on_rope_pos = mathf::max(0.f, p.on_rope_pos - 0.016666668f);
            else
                p.on_rope_pos = mathf::min(1.f, p.on_rope_pos + 0.016666668f);
            p.on_rope_pos = mathf::lerp(p.on_rope_pos, 0.5f, mathf::inverse_lerp(100.f, 200.f, float(p.tried_move)));
            ivec2 const here = room_view::get_tile_position(x.pos(0));
            ivec2 start = here;
            std::optional<ivec2> dest;
            static constexpr ivec2 four_and_zero[5] = {{0, 0}, {-1, 0}, {0, -1}, {1, 0}, {0, 1}};
            for (uint32_t k = 0; k < 5 && !dest; ++k) {
                start = here + four_and_zero[k];
                dest = follow_path(x, start);
            }
            if (dest) {
                if (p.tried_move > 40)
                    x.vel(s) += custom::dir_vec(x.pos(s), room_view::middle_of_tile(*dest));
                physics::set_go_through_floors(x.state, body(x.state, x.c), dest->y < start.y);
            }
            // close to the pipe it heads for: the tongues let go before it
            // squeezes in (a ShortCut connection, dens.hpp pulls it in)
            if (w.creature_get_going_home(x.c) && custom::dist_less(x.pos(0), room_view::middle_of_tile(w.creature_get_travel_tile(x.c)), 60.f))
                for (uint32_t k = 0; k < 2; ++k) {
                    release(x, k);
                    x.t(k).rope.bend_count = 0;
                }
            if (!custom::dist_less(x.pos(o), x.t(o).pos, 20.f))
                return;
            float const side = x.rng.value() < 0.5f ? -1.f : 1.f;
            for (auto const dv : custom::four_directions)
                if (x.room.get_tile(x.pos(o) + vec2{float(dv.x) * 20.f * side, float(dv.y) * 20.f}).solid())
                    x.vel(s) -= vec2{float(dv.x) * side, float(dv.y)} * custom::lerp_map(float(p.tried_move), 20.f, 100.f, 1.2f, 1.5f);
            x.vel(o) += custom::dir_vec(x.pos(o), x.t(o).pos) * 0.6f;
            if (x.t(s).mode != tongue_mode::retracted || !dest)
                return;
            // at the wall beside the next tile, if there is one
            vec2 to = room_view::middle_of_tile(*dest);
            for (int32_t m = 3; m >= 0; --m)
                if (x.room.get_tile(*dest + custom::four_directions[m]).solid()) {
                    to = room_view::middle_of_tile(*dest) + rw::to_vec2(custom::four_directions[m]) * 11.f;
                    break;
                }
            if (swing_end_at(x, to, 1.f)) {
                p.tried_move = 0;
                ++p.shoot_tries;
            }
        }

        void act(ctx& x) {
            auto& w = x.w();
            auto& p = x.x;
            if (x.t(0).mode == tongue_mode::attached_to_object || x.t(1).mode == tongue_mode::attached_to_object) {
                hold_on_to_object(x);
                return;
            }
            ivec2 const at = room_view::get_tile_position(x.pos(p.step));
            if (x.accessible(at))
                w.tube_worm_data_set_last_accessible(x.d, at);
            ai_update(x);
            ++p.tried_move;
            ++p.grappling_counter;
            bool const accessible = x.accessible(at);
            if (x.rng.value() < 0.025f && x.t(0).is_attached() && x.t(1).is_attached()
            && pathing::ai_terrain_proximity(x.state, x.room.id, room_view::get_tile_position(x.pos(0))) > 2) {
                p.sleeping = 1;
                p.sleep_counter = x.rng.range(60, 1260);
                p.goal_on_rope_pos = x.rng.value();
                return;
            }
            if (w.tube_worm_data_get_grapple_to_pos(x.d) && x.t(1 - p.step).is_attached() && (p.grappling_counter > 40 || !accessible)
            && !pathing::narrow_space(x.room, at)) {
                grapple_to_position(x);
                return;
            }
            if (accessible)
                find_inaccessible_grappling_pos(x);
            flop_along(x);
        }

        // GrabbedByPlayer: the front tongue shoots from the slugcat; its jump
        // asked for it (useBool)
        void grabbed_by_player(ctx& x) {
            auto& w = x.w();
            auto& p = x.x;
            if (!p.last_grabbed) {
                x.t(0).elastic = 1.f;
                x.t(1).elastic = 1.f;
            }
            p.sleeping = 0;
            x.base[0] = player::body_chunk(x.state, x.holder, 0);
            if (x.t(1).is_attached())
                release(x, 1);
            p.on_rope_pos = mathf::min(1.f, p.on_rope_pos + 0.05f);
            vec2 dir = rw::normalized(vec2{float(w.player_get_flip_direction(x.holder)), 0.7f});
            if (w.player_get_input(x.holder)[0].y > 0)
                dir = vec2{0.f, 1.f};
            x.vel(0) += dir * 3.f;
            x.vel(1) -= dir * 3.f;
            auto const pc = player::creature_of(x.state, x.holder);
            if (w.creature_get_entering_shortcut(pc).x >= 0 || w.creature_get_away(pc) != 0) {
                for (uint32_t j = 0; j < 2; ++j)
                    if (x.t(j).is_attached())
                        release(x, j);
                return;
            }
            // (no Remix: no vine special case)
            if (p.use_bool && !w.creature_get_dead(x.c))
                shoot(x, 0, dir);
            p.use_bool = 0;
        }

        // the slugcat holding it (grabbedBy[i].grabber is Player)
        dcon::player_id holding_slugcat(sys::game_state const& state, dcon::creature_id c) {
            dcon::player_id out;
            creature::for_each_holder(state, creature::room_of(state, c), c, [&](dcon::creature_id h, uint32_t) {
                if (!out && creature::is_slugcat(state.world, h))
                    out = creature::player_of(state, h);
            });
            return out;
        }

        void reset_tongues(sys::game_state& state, dcon::creature_id c, vec2 at) {
            auto& tp = state.world.tube_worm_data_get_tongues(data(state, c));
            for (auto& t : tp.t) {
                t.mode = tongue_mode::retracted;
                t.attached = dcon::creature_id{};
                t.pos = t.last_pos = at;
                t.vel = vec2{0.f, 0.f};
                reset_rope(t.rope, at);
            }
        }
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::tube_worm);
        auto const d = data(state, c);
        auto rng = rw::random(state.seed, 0, uint32_t(c.value));
        int32_t seed = int32_t(rng.next() & 0x7FFFFFFFu);
        if (state.spawn_seed >= 0)
            seed = state.spawn_seed;
        physics::object_desc desc;
        desc.gravity = 0.9f;
        desc.air_friction = 0.999f;
        desc.bounce = 0.1f;
        desc.surface_friction = 0.4f;
        desc.water_friction = 0.96f;
        desc.buoyancy = 0.95f;
        auto const o = physics::create_object(state, room, desc);
        // two chunks of 6, 0.1 each, 7 apart
        auto const c0 = physics::add_chunk(state, o, at, 6.f, 0.1f);
        auto const c1 = physics::add_chunk(state, o, at + vec2{0.f, -1.f}, 6.f, 0.1f);
        physics::add_connection(state, o, c0, c1, 7.f, physics::connection_type::normal, 1.f, 0.5f);
        w.try_create_creature_body(c, o);
        auto& x = pl(state, c);
        x.lungs = 0.f;
        x.crawl_speed = 1.f;
        x.walk_cycle = rng.value();
        x.last_walk = x.walk_cycle;
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_set_grasps(c, creature::grasp_pair{});
        w.creature_set_destination(c, room_view::get_tile_position(at));
        w.tube_worm_data_set_seed(d, seed);
        w.tube_worm_data_set_tongues(d, tongue_pair{});
        w.tube_worm_data_set_grapple_to_pos(d, std::nullopt);
        w.tube_worm_data_set_next_grapple_pos(d, std::nullopt);
        w.tube_worm_data_set_last_accessible(d, std::nullopt);
        w.tube_worm_data_set_player_cheat_attach_pos(d, std::nullopt);
        w.tube_worm_data_set_dest_counter(d, 0);
        reset_tongues(state, c, at);
        tube_worm_graphics::reset(state, c);
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        creature::release_target(state, c);
        creature::erase(state, c);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        ctx x(state, c, room);
        auto& p = x.x;
        p.last_walk = p.walk_cycle;
        x.holder = holding_slugcat(state, c);
        bool const held = creature::held(state, c);
        for (uint32_t i = 0; i < 2; ++i)
            w.body_chunk_set_collide_with_terrain(x.ch[i], !held);
        if (p.sleeping)
            p.crawl_speed = mathf::max(0.4f, p.crawl_speed - 0.0025f);
        else
            p.crawl_speed = mathf::min(1.4f, p.crawl_speed + 0.005f);
        // Creature.Update: the stun counts down, a slugcat's hold keeps it
        // pacified (stun 10)
        creature::stun_countdown(state, c);
        if (x.holder && w.creature_get_stun(c) < 10)
            w.creature_set_stun(c, 10);
        physics::update_object(state, room, body(state, c), nullptr, nullptr);
        if (x.holder)
            grabbed_by_player(x);
        else if (consious(state, c)) {
            if (p.sleeping)
                sleep(x);
            else
                act(x);
        }
        p.last_grabbed = x.holder ? 1 : 0;
        bool both = true;
        for (uint32_t j = 0; j < 2; ++j) {
            tongue_update(x, j);
            if (!x.t(j).is_attached())
                both = false;
        }
        if (both)
            w.physical_object_for_each_chunk_connection_as_owner(body(state, c), [&](dcon::chunk_connection_id k) { physics::update_connection(state, k); });
        // drowning under the water
        if (!w.creature_get_dead(c) && physics::submersion(state, room, x.ch[0]) > 0.5f) {
            p.lungs = mathf::max(p.lungs - 0.0055555557f, 0.f);
            if (p.lungs == 0.f)
                creature::die(state, c);
        } else {
            p.lungs = mathf::min(p.lungs + 0.02f, 1.f);
        }
        // overstretched: a tongue lets go
        float const ropes = x.t(0).rope.total_length + x.t(1).rope.total_length;
        if ((x.rng.value() < 0.033333335f && ropes > max_total_rope * 2.f) || ropes > max_total_rope * 5.f)
            release(x, uint32_t(x.rng.range(0, 2)));
    }

    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) {
        return !creature::holds(state.world, t, c);
    }

    void collide(sys::game_state& state, dcon::creature_id c) {
        pl(state, c).sleeping = 0;
    }

    void violence(sys::game_state& state, dcon::creature_id c, uint32_t k, vec2 momentum, float damage, float stun_bonus) {
        auto& w = state.world;
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        {
            auto const room = world::get_room_view(state, creature::room_of(state, c));
            ctx x(state, c, room);
            for (uint32_t i = 0; i < 2; ++i)
                if (rng.value() < 0.5f)
                    release(x, i);
        }
        // Creature.Violence (HealthState)
        auto const hit = chunk(state, c, k);
        w.body_chunk_get_vel(hit) += rw::clamp_magnitude(momentum / w.body_chunk_get_mass(hit), 10.f);
        float const dmg = damage / base_damage_resistance;
        float st = (damage * 30.f + stun_bonus) / base_stun_resistance;
        float& health = w.creature_get_health(c);
        st *= 1.5f + mathf::inverse_lerp(0.5f, 0.f, health) * rng.value();
        creature::stun(state, c, int32_t(st));
        health -= dmg;
        if (rng.value() < -health || health < -1.f || (health < 0.f && rng.value() < 0.33f) || dmg >= instant_death_damage_limit)
            creature::die(state, c);
    }

    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t) {
        auto& tp = state.world.tube_worm_data_get_tongues(data(state, c));
        for (auto& tg : tp.t)
            if (tg.mode == tongue_mode::attached_to_object && tg.attached == t) {
                tg.mode = tongue_mode::retracting;
                tg.attached = dcon::creature_id{};
            }
    }

    void died(sys::game_state& state, dcon::creature_id c) {
        if (auto const p = holding_slugcat(state, c))
            release_player(state, p);
        auto const room = world::get_room_view(state, creature::room_of(state, c));
        ctx x(state, c, room);
        for (uint32_t i = 0; i < 2; ++i)
            if (x.t(i).is_attached())
                release(x, i);
    }

    void place_in_room(sys::game_state& state, dcon::creature_id c, vec2 at) {
        reset_tongues(state, c, at);
        state.world.tube_worm_data_set_grapple_to_pos(data(state, c), std::nullopt);
        state.world.tube_worm_data_set_next_grapple_pos(data(state, c), std::nullopt);
        state.world.tube_worm_data_set_last_accessible(data(state, c), std::nullopt);
        // TubeWormAI.NewRoom
        state.world.tube_worm_data_set_dest_counter(data(state, c), 0);
    }

    tongue const& tongue_of(sys::game_state const& state, dcon::creature_id c, uint32_t i) noexcept {
        return state.world.tube_worm_data_get_tongues(data(state, c)).t[i & 1];
    }

    float total_rope(sys::game_state const& state, dcon::creature_id c) noexcept {
        return total_rope(state.world.tube_worm_data_get_tongues(data(state, c)));
    }

    float rope_stretch_fac(sys::game_state const& state, dcon::creature_id c) noexcept {
        return rope_stretch(state.world.tube_worm_data_get_tongues(data(state, c)), pl(state, c).on_rope_pos);
    }

    std::string describe_tongues(sys::game_state const& state, dcon::creature_id c) {
        static constexpr char const* names[] = {"in", "out", "stuck", "on a creature", "back"};
        auto const& tp = state.world.tube_worm_data_get_tongues(data(state, c));
        return std::string{names[uint32_t(tp.t[0].mode)]} + "/" + names[uint32_t(tp.t[1].mode)];
    }

    dcon::creature_id held_in_hand(sys::game_state const& state, dcon::player_id p, uint32_t hand) noexcept {
        if (!p)
            return dcon::creature_id{};
        auto const c = creature::grasped(state.world, player::creature_of(state, p), hand);
        return c && creature::type_of(state.world, c) == creature::type::tube_worm ? c : dcon::creature_id{};
    }

    dcon::creature_id held_by(sys::game_state const& state, dcon::player_id p) noexcept {
        if (auto const c = held_in_hand(state, p, 0))
            return c;
        return held_in_hand(state, p, 1);
    }

    dcon::creature_id pickup_candidate(sys::game_state const& state, dcon::player_id p, room_view const& room, float& score) {
        auto const& w = state.world;
        score = 3.4e38f;
        // Player.Grabability: CantGrab while it holds one
        if (held_by(state, p))
            return dcon::creature_id{};
        vec2 const p0 = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
        int32_t const flip = w.player_get_flip_direction(p);
        dcon::creature_id result;
        rooms::for_each_creature(state, room.id, creature::type::tube_worm, [&](dcon::creature_id c) {
            if (creature::held(state, c) || w.creature_get_away(c) != 0)
                return;
            for (uint32_t i = 0; i < 2; ++i) {
                auto const k = chunk(state, c, i);
                vec2 const cp = w.body_chunk_get_pos(k);
                float const rad = w.body_chunk_get_rad(k);
                if (!custom::dist_less(p0, cp, rad + 40.f))
                    continue;
                if (!custom::dist_less(p0, cp, rad + 20.f) && !room.visual_contact(p0, cp))
                    continue;
                float dist = rw::distance(p0, cp);
                if ((cp.x < p0.x) == (flip < 0))
                    dist -= 10.f;
                if (dist < score) {
                    result = c;
                    score = dist;
                }
            }
        });
        return result;
    }

    void grab(sys::game_state& state, dcon::player_id p, dcon::creature_id c, uint32_t hand) {
        creature::grab(state, player::creature_of(state, p), hand, c, 0);
    }

    void release(sys::game_state& state, dcon::creature_id c) {
        uint32_t g = 0;
        if (auto const h = creature::holder(state, c, &g))
            creature::release_grasp(state, h, g);
    }

    void release_player(sys::game_state& state, dcon::player_id p) {
        for (uint32_t hand = 0; hand < 2; ++hand)
            if (held_in_hand(state, p, hand))
                creature::release_grasp(state, player::creature_of(state, p), hand);
    }

    void carry_update(sys::game_state& state, dcon::player_id p, uint32_t hand, vec2 hand_pos) {
        auto& w = state.world;
        auto const c = held_in_hand(state, p, hand);
        if (!c)
            return;
        auto const k = chunk(state, c, 0);
        w.body_chunk_set_last_pos(k, w.body_chunk_get_pos(k));
        w.body_chunk_set_pos(k, hand_pos);
        w.body_chunk_set_vel(k, w.body_chunk_get_vel(player::body_chunk(state, p, 0)));
    }

    void follow_player(sys::game_state& state, dcon::player_id p, dcon::room_id room, bool spit_out) {
        auto& w = state.world;
        for (uint32_t hand = 0; hand < 2; ++hand) {
            auto const c = held_in_hand(state, p, hand);
            if (!c)
                continue;
            w.physical_object_set_room_from_object_location(body(state, c), room);
            if (!spit_out)
                continue;
            vec2 const at = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
            for (uint32_t i = 0; i < 2; ++i) {
                physics::hard_set_position(state, chunk(state, c, i), at);
                w.body_chunk_set_vel(chunk(state, c, i), w.body_chunk_get_vel(player::body_chunk(state, p, 0)));
            }
            place_in_room(state, c, at);
            tube_worm_graphics::reset_positions(state, c);
        }
    }

    bool jump_button(sys::game_state& state, dcon::creature_id c, dcon::player_id p) {
        auto& w = state.world;
        // (no Remix: a worm stunned past its pacified 10 doesn't take it)
        if (w.creature_get_dead(c) || w.creature_get_stun(c) > 12)
            return true;
        auto& x = pl(state, c);
        if (w.player_get_can_jump(p) < 1 && w.player_get_body_mode(p) == player::body_mode_index::default_) {
            x.use_bool = 1;
            return false;
        }
        if (tongue_of(state, c, 0).is_attached()) {
            x.use_bool = 1;
            return false;
        }
        return true;
    }

    bool swinging(sys::game_state const& state, dcon::creature_id c) noexcept {
        return tongue_of(state, c, 0).is_attached();
    }

    vec2 attached_pos(sys::game_state const& state, dcon::creature_id c) noexcept {
        return tongue_of(state, c, 0).terrain_stuck_pos;
    }
}
