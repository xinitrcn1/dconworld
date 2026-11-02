// BigSpider.cs + BigSpiderAI (the big spider, the spitter spider and
// Downpour's mother spider), simplified where noted (big_spider.hpp)

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "big_spider.hpp"
#include "big_spider_graphics.hpp"
#include "dart_maggot.hpp"
#include "dens.hpp"
#include "items.hpp"
#include "migrate.hpp"
#include "particles.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "rooms.hpp"
#include "tracker.hpp"

namespace big_spider {
    using creature::relationship;
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // CreatureTemplate: baseDamageResistance, baseStunResistance,
        // instantDeathDamageLimit, visualRadius (BigSpider, SpitterSpider,
        // MotherSpider)
        struct template_params {
            float base_damage_resistance;
            float base_stun_resistance;
            float instant_death_damage_limit;
            float visual_radius;
        };
        constexpr template_params templates[variant_count] = {
            {0.6f, 0.5f, 1.f, 500.f},
            {1.1f, 0.6f, 1.f, 1100.f},
            {1.1f, 0.6f, 1.f, 300.f},
        };

        creature::big_spider_payload& pl(sys::game_state& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).big_spider;
        }
        creature::big_spider_payload const& pl(sys::game_state const& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).big_spider;
        }
        dcon::big_spider_data_id data(sys::game_state const& state, dcon::creature_id c) {
            return creature::big_spider_data(state.world, c);
        }
        move_state& mv_of(sys::game_state& state, dcon::creature_id c) {
            return state.world.big_spider_data_get_move(data(state, c));
        }
        ai_state& ai_of(sys::game_state& state, dcon::creature_id c) {
            return state.world.big_spider_data_get_ai(data(state, c));
        }
        spit_state& spit_of(sys::game_state& state, dcon::creature_id c) {
            return state.world.big_spider_data_get_spit(data(state, c));
        }
        template_params const& tp(sys::game_state const& state, dcon::creature_id c) {
            return templates[uint32_t(pl(state, c).variant) % variant_count];
        }
        vec2& pos(sys::game_state& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_pos(chunk(state, c, i));
        }
        vec2 pos(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_pos(chunk(state, c, i));
        }
        vec2& vel(sys::game_state& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_vel(chunk(state, c, i));
        }
        ivec2 tile_of(vec2 p) {
            return room_view::get_tile_position(p);
        }
        // abstractCreature.pos: the main chunk's tile
        ivec2 here(sys::game_state const& state, dcon::creature_id c) {
            return tile_of(pos(state, c, 0));
        }
        // AImap.TileAccessibleToCreature
        bool accessible(world::room_view const& room, ivec2 t) {
            return room.inside(t.x, t.y) && pathing::accessible(room, t, spider_movement);
        }
        bool accessible(world::room_view const& room, vec2 p) {
            return accessible(room, tile_of(p));
        }
        bool is_big_spider(sys::game_state const& state, dcon::creature_id t) {
            return creature::live(state, t) && state.world.creature_get_type(t) == creature::type::big_spider;
        }
        float mass(sys::game_state const& state, dcon::creature_id c) {
            return creature::total_mass(state, c);
        }

        // Tracker(10, 10, 1500, 0.5, 5, 5, 10), ThreatTracker(3),
        // PreyTracker(5, 2, 10, 70, 0.5)
        tracker::params const& tracker_params() {
            static tracker::params const p = [] {
                tracker::params q;
                q.see_around_corners = 10;
                q.frames_to_remember = 1500;
                q.ghost_dismissal_range = 10;
                q.prey_max = 5;
                q.persistance_bias = 2.f;
                q.sure_to_get = 10.f;
                q.sure_to_lose = 70.f;
                q.threat_max = 3;
                return q;
            }();
            return p;
        }

        //
        // little things
        //
        // BigSpider.ReleaseAllGrabChunks
        void release_all_grab_chunks(sys::game_state& state, dcon::creature_id c) {
            for (auto& side : mv_of(state, c).grab)
                for (auto& g : side)
                    g = grab_chunk{};
        }
        void lose_all_grasps(sys::game_state& state, dcon::creature_id c) {
            creature::release_grasp(state, c, 0);
        }
        // BigSpider.Stun's own part (the stun itself is Creature's)
        void on_stun(sys::game_state& state, dcon::creature_id c, int32_t st, rw::rng& rng) {
            if (st > 4 && rng.value() < 0.5f)
                lose_all_grasps(state, c);
            mv_of(state, c).reviving_buddy = dcon::creature_id{};
            release_all_grab_chunks(state, c);
        }
        void stun(sys::game_state& state, dcon::creature_id c, int32_t st, rw::rng& rng) {
            creature::stun(state, c, st);
            on_stun(state, c, st, rng);
        }
        bool can_jump(sys::game_state const& state, dcon::creature_id c) {
            return pl(state, c).jump_stamina >= 0.2f && !creature::grasped_object(state.world, c, 0);
        }
        // CarryBackwards (no StuckTracker: its utility is 0)
        bool carry_backwards(sys::game_state& state, dcon::creature_id c) {
            return mv_of(state, c).carry_object_mass > 0.f;
        }
        // LegsGrabby
        bool legs_grabby(sys::game_state const& state, dcon::creature_id c) {
            return (spitter(state, c) || pl(state, c).can_cling > 0) && !creature::grasped_object(state.world, c, 0);
        }
        // CanIBeRevived (out in this room, not too far gone, nobody holds it;
        // never a mother)
        bool can_be_revived(sys::game_state const& state, dcon::creature_id t, dcon::room_id room) {
            return is_big_spider(state, t) && creature::here(state, t, room) && state.world.creature_get_health(t) > -8.f && !creature::held(state, t)
                && !mother(state, t);
        }
        // BigSpider.Revive
        void revive(sys::game_state& state, dcon::creature_id t) {
            auto& w = state.world;
            w.creature_set_dead(t, 0);
            w.creature_set_destination(t, here(state, t));
            pl(state, t).death_convulsions = 1.f;
        }
        // a slugcat or a creature holding a spear (SpiderTrackState.armed)
        bool holds_spear(sys::game_state const& state, dcon::creature_id t) {
            auto const& w = state.world;
            auto const spear = [](world::item_type ty) {
                return ty == world::item_type::spear || ty == world::item_type::explosive_spear || ty == world::item_type::electric_spear;
            };
            if (creature::is_slugcat(w, t)) {
                auto const p = creature::player_of(state, t);
                if (!p)
                    return false;
                for (uint32_t h = 0; h < 2; ++h)
                    if (auto const it = items::held(state, p, h); it && spear(w.item_get_type(it)))
                        return true;
                return false;
            }
            for (uint32_t h = 0; h < 2; ++h)
                if (auto const it = creature::grasped_item(w, t, h); it && spear(w.item_get_type(it)))
                    return true;
            return false;
        }

        //
        // BigSpiderAI's SpiderTrackStates
        //
        track_state* find_track(ai_state& ai, dcon::creature_id who) {
            for (auto& t : ai.track)
                if (t.who == who)
                    return &t;
            return nullptr;
        }
        // the state of a tracked creature (a fresh one for a new one, in the
        // slot of one the tracker forgot)
        track_state& track_of(ai_state& ai, creature::tracker_state const& ts, dcon::creature_id who) {
            if (auto* t = find_track(ai, who))
                return *t;
            for (auto& t : ai.track)
                if (!t.who || !tracker::find(ts, t.who)) {
                    t = track_state{};
                    t.who = who;
                    return t;
                }
            ai.track[0] = track_state{};
            ai.track[0].who = who;
            return ai.track[0];
        }
        // SpiderTrackState.ReadyForSpitter
        bool ready_for_spitter(track_state const& t) {
            return !t.consious || t.tagged > 0;
        }

        //
        // the hooks the tracker asks
        //
        struct hook_ctx {
            sys::game_state& state;
            dcon::creature_id c;
            room_view const& room;
            std::optional<pathing::field_id> from_here;
            rw::rng rng;
        };
        bool reachable(hook_ctx& x, ivec2 t) {
            if (!accessible(x.room, t))
                return false;
            if (!x.from_here)
                x.from_here = pathing::field_to(x.state, x.room.id, here(x.state, x.c), spider_movement);
            return pathing::distance(x.state, *x.from_here, t) != pathing::unreachable;
        }
        // ArtificialIntelligence.VisualContact
        bool hook_sees_point(void* u, vec2 p) {
            auto& x = *static_cast<hook_ctx*>(u);
            return custom::dist_less(pos(x.state, x.c, 0), p, tp(x.state, x.c).visual_radius) && x.room.visual_contact(pos(x.state, x.c, 0), p);
        }
        bool hook_sees(void* u, dcon::body_chunk_id k) {
            auto& x = *static_cast<hook_ctx*>(u);
            return hook_sees_point(u, x.state.world.body_chunk_get_pos(k));
        }
        // BigSpiderAI.UpdateDynamicRelationship (no light shyness:
        // ShyFromLight is 0, see big_spider.hpp)
        relationship hook_relate(void* u, creature::tracked const& r) {
            auto& x = *static_cast<hook_ctx*>(u);
            auto& state = x.state;
            auto const c = x.c;
            auto& w = state.world;
            auto& ai = ai_of(state, c);
            auto const& ts = tracker::of(state, c);
            auto& st = track_of(ai, ts, r.who);
            bool const sp = spitter(state, c);
            if (r.visual) {
                st.alive = !creature::dead(state, r.who);
                if (creature::live(state, r.who)) {
                    st.consious = consious(state, r.who);
                    auto const ty = w.creature_get_type(r.who);
                    if (sp && (ty == creature::type::slugcat || ty == creature::type::scavenger))
                        st.armed = holds_spear(state, r.who);
                    if (st.total_mass == 1.f)
                        st.total_mass = creature::is_slugcat(w, r.who) ? 0.84f : creature::mass_of(state, r.who);
                }
            }
            auto const them = creature::template_of(state, r.who);
            auto const me = creature::template_of(state, c);
            relationship rel = creature::static_relationship(me, them);
            float const my_mass = mass(state, c);
            if (them == creature::tmpl::centipede && st.consious && st.total_mass > my_mass * 2.f)
                rel = relationship{creature::rel::afraid, mathf::inverse_lerp(my_mass, my_mass * 7.f, st.total_mass)};
            if (sp) {
                auto const& sm = spit_of(state, c);
                if (st.tagged > 0)
                    --st.tagged;
                if (rel.type == creature::rel::eats && (creature::static_relationship(them, me).type == creature::rel::eats || st.armed)) {
                    st.close = rw::distance(rw::to_vec2(here(state, c)), rw::to_vec2(tracker::best_guess(r))) < (st.close ? 10.f : 6.f);
                    if (st.consious) {
                        if (st.close)
                            rel = relationship{creature::rel::afraid, 0.6f};
                        else if (st.tagged > 200 && st.tagged < until_sleep_delay + 160)
                            rel = relationship{creature::rel::ignores, 0.f};
                    }
                }
                if (sm.random_crit_spit_delay > 0 && r.who == sm.spit_at)
                    rel.type = creature::rel::attacks;
            }
            if (rel.type == creature::rel::afraid) {
                if (!st.alive)
                    rel.intensity = 0.f;
            } else if (rel.type == creature::rel::eats || rel.type == creature::rel::attacks) {
                if (sp) {
                    auto const& sm = spit_of(state, c);
                    if (sm.tagged) {
                        if (r.who == sm.tagged)
                            rel.intensity = mathf::lerp(rel.intensity, 1.f, 0.5f);
                        else
                            rel.intensity *= 0.2f;
                    }
                    return rel;
                }
                if (r.visual)
                    st.accustomed += 2;
                else if (r.ticks_since_seen < 300)
                    st.accustomed += 1;
                else
                    st.accustomed -= 1;
                st.accustomed = custom::int_clamp(st.accustomed, 0, 1800);
                float num = 0.f;
                if (!ai.stay_away) {
                    float const others = float(std::max<int32_t>(1, int32_t(ai.other_spider_count)));
                    float const borrowed = pl(state, c).borrowed_time > 0 ? 5.f : 1.f;
                    num = mathf::inverse_lerp(200.f, custom::lerp_map(st.total_mass, 0.1f, 4.f, 200.f, 1800.f, 0.29f), float(st.accustomed) * others * borrowed);
                    if (!st.consious)
                        num = 1.f;
                }
                if (num < 0.5f)
                    rel = relationship{creature::rel::afraid, mathf::inverse_lerp(0.5f, 0.f, num) * mathf::pow(mathf::inverse_lerp(0.2f, 4.f, creature::body_size(them)), 0.5f)};
                else
                    rel.intensity *= mathf::inverse_lerp(0.5f, 0.75f, num);
            }
            return rel;
        }
        bool hook_reach(void* u, ivec2 t) {
            return reachable(*static_cast<hook_ctx*>(u), t);
        }
        tracker::hooks make_hooks(hook_ctx& x) {
            tracker::hooks h;
            h.user = &x;
            h.sees = hook_sees;
            h.sees_point = hook_sees_point;
            h.relate = hook_relate;
            h.module = tracker::module_without_aggression;
            h.reach = hook_reach;
            return h;
        }

        //
        // BigSpider's moves
        //
        // StandardPather.FollowPath from the tile `from`
        movement_connection follow_path(sys::game_state const& state, pathing::field_id f, ivec2 from) {
            auto const step = pathing::next_step(state, f, from);
            if (!step)
                return movement_connection{};
            return movement_connection{from, *step, true};
        }

        // BigSpider.Jump
        void jump(sys::game_state& state, dcon::creature_id c, vec2 dir, float) {
            auto& x = pl(state, c);
            auto& mv = mv_of(state, c);
            float const d = custom::lerp_map(dir.y, -1.f, 1.f, 0.7f, 1.2f, 1.1f);
            x.footing_counter = 0;
            vel(state, c, 0) *= 0.5f;
            vel(state, c, 1) *= 0.5f;
            vel(state, c, 0) += dir * 16.f * d;
            vel(state, c, 1) += dir * 11.f * d;
            x.charging = 0.f;
            x.jumping = 1;
            // the legs let go and fling (BigSpiderGraphics, from these)
            ++mv.jump_count;
            mv.jump_dir = dir;
            // (no Big_Spider_Jump sound)
        }

        // BigSpider.InitiateJump
        void initiate_jump(sys::game_state& state, dcon::creature_id c, vec2 target) {
            auto& x = pl(state, c);
            if (x.charging > 0.f || x.jumping || !can_jump(state, c))
                return;
            x.charging = 0.01f;
            mv_of(state, c).jump_at_pos = target;
            // (no Big_Spider_Jump_Warning_Rustle sound)
        }

        // SpiderSpitModule.CanSpit
        bool module_can_spit(sys::game_state& state, dcon::creature_id c) {
            auto& sm = spit_of(state, c);
            if (sm.ammo <= 0 || !sm.spit_at)
                return false;
            auto const* r = tracker::find(tracker::of(state, c), sm.spit_at);
            if (!r || !r->visual || sm.target_trail_count < target_trail_max)
                return false;
            auto const* t = find_track(ai_of(state, c), sm.spit_at);
            int32_t const tagged = t ? t->tagged : 0;
            return tagged < 1 || tagged > until_sleep_delay + 80;
        }
        // BigSpider.CanSpit
        bool can_spit(sys::game_state& state, dcon::creature_id c, bool initiate) {
            return consious(state, c) && footing(state, c) && !creature::grasped_object(state.world, c, 0) && module_can_spit(state, c)
                && rw::dot(custom::dir_vec(pos(state, c, 1), pos(state, c, 0)), spit_of(state, c).aim_dir) > (initiate ? 0.6f : 0.3f);
        }
        // BigSpider.TryInitiateSpit
        void try_initiate_spit(sys::game_state& state, dcon::creature_id c) {
            auto& x = pl(state, c);
            if (x.charging > 0.f || !can_spit(state, c, true))
                return;
            x.charging = 0.01f;
            // (no Big_Spider_Spit_Warning_Rustle sound)
        }
        // SpiderSpitModule.SpiderHasSpit
        void spider_has_spit(sys::game_state& state, dcon::creature_id c) {
            auto& sm = spit_of(state, c);
            --sm.ammo;
            sm.ammo_regen = 0.f;
            if (sm.ammo < 1) {
                sm.fast_ammo_regen = true;
                ai_of(state, c).stay_away = true;
                sm.random_crit_spit_delay = 0;
                return;
            }
            sm.fast_ammo_regen = false;
        }
        // BigSpider.Spit: a DartMaggot (an item, made after the rooms' update)
        void spit(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& x = pl(state, c);
            vec2 const v = spit_of(state, c).aim_dir;
            x.charging = 0.f;
            pos(state, c, 0) += v * 12.f;
            vel(state, c, 0) += v * 2.f;
            vec2 const at = pos(state, c, 0);
            state.defer(room.id, [at, v, c, rid = room.id](sys::game_state& s) {
                auto const it = items::create(s, rid, at, world::item_type::dart_maggot);
                dart_maggot::shoot(s, it, at, v, c);
            });
            // (no Big_Spider_Spit sound)
            spider_has_spit(state, c);
        }

        // BigSpider.Attack: the charged pounce goes off
        void attack(sys::game_state& state, dcon::creature_id c, room_view const& room, rw::rng& rng) {
            auto& x = pl(state, c);
            auto& mv = mv_of(state, c);
            auto const* prey = tracker::prey(tracker::of(state, c));
            vec2 const main = pos(state, c, 0);
            if (!prey || !can_jump(state, c) || !prey->visual || !room.visual_contact(main, mv.jump_at_pos) || !creature::here(state, prey->who, room.id)) {
                x.charging = 0.f;
                return;
            }
            vec2 v = custom::dir_vec(main, mv.jump_at_pos);
            vec2 v2 = creature::main_pos(state, prey->who);
            v2 += vec2{0.f, mathf::inverse_lerp(40.f, 300.f, rw::distance(main, v2)) * 40.f};
            if (!custom::dist_less(main, v2, custom::lerp_map(rw::dot(v, custom::dir_vec(main, v2)), -1.f, 1.f, 0.f, 500.f))) {
                x.charging = 0.f;
                return;
            }
            x.jump_stamina = mathf::max(0.f, x.jump_stamina - 0.35f);
            if (x.jump_stamina < 0.2f && rng.value() < 0.5f && !spitter(state, c))
                ai_of(state, c).stay_away = true;
            if (!room.get_tile(main + vec2{0.f, 20.f}).solid() && !room.get_tile(pos(state, c, 1) + vec2{0.f, 20.f}).solid())
                v = rw::slerp(v, vec2{0.f, 1.f}, custom::lerp_map(rw::distance(main, mv.jump_at_pos), 40.f, 400.f, 0.2f, 0.5f));
            jump(state, c, v, 1.f);
            x.can_bite = 40;
        }

        // BigSpider.MoveTowards
        void move_towards(sys::game_state& state, dcon::creature_id c, room_view const& room, vec2 to, rw::rng& rng) {
            auto& x = pl(state, c);
            if (rng.value() > state.world.creature_get_health(c))
                return;
            vec2 dir = custom::dir_vec(pos(state, c, 0), to);
            if (!footing(state, c))
                dir *= 0.3f;
            bool const back = carry_backwards(state, c);
            if (!back && physics::is_tile_solid(state, room, chunk(state, c, 1), 0, -1)
            && ((dir.x < -0.5f && pos(state, c, 0).x > pos(state, c, 1).x + 5.f) || (dir.x > 0.5f && pos(state, c, 0).x < pos(state, c, 1).x - 5.f))) {
                float const s = dir.x < 0.f ? -1.f : 1.f;
                vel(state, c, 0).x -= s * 1.3f;
                vel(state, c, 1).x += s * 0.5f;
                if (!physics::is_tile_solid(state, room, chunk(state, c, 0), 0, 1))
                    vel(state, c, 0).y += 3.2f;
            }
            bool const sp = spitter(state, c);
            float const d = custom::lerp_map(mv_of(state, c).carry_object_mass, 0.f, sp ? 24.f : 14.f, 1.f, 0.2f + 0.8f * x.stuck_shake)
                * mathf::lerp(0.5f, 1.f, x.run_speed) * mathf::lerp((sp && !back) ? 0.7f : 1.25f, 1.5f, x.stuck_shake);
            if (back) {
                vel(state, c, 1) += dir * 3.1f * d;
                vel(state, c, 0) -= dir * 1.9f * d;
                physics::set_go_through_floors(state, body(state, c), to.y < pos(state, c, 1).y - 5.f);
                return;
            }
            vel(state, c, 0) += dir * 4.1f * d;
            vel(state, c, 1) -= dir * 0.9f * d;
            physics::set_go_through_floors(state, body(state, c), to.y < pos(state, c, 0).y - 5.f);
        }

        // BigSpider.Run. The path fields' steps are Standard moves, open
        // diagonals (a special move) and drops (DropToFloor); shortcuts are
        // dens.hpp's
        void run(sys::game_state& state, dcon::creature_id c, room_view const& room, pathing::field_id f, movement_connection const& following, rw::rng& rng) {
            auto& x = pl(state, c);
            auto& mv = mv_of(state, c);
            ivec2 const step = following.dest - following.start;
            if (step.x != 0 && step.y != 0) {
                x.special_move_counter = 30;
                mv.special_move_destination = following.dest;
                mv.last_followed = following;
                return;
            }
            movement_connection con = following;
            // a step further on, if the way there is all open to it
            if (auto const next = follow_path(state, f, following.dest); next.valid) {
                if (next.dest == following.start) {
                    x.sitting = 1;
                    return;
                }
                if (room.inside(next.dest.x, next.dest.y) && pathing::tile_access(room, next.dest) < pathing::access::ceiling) {
                    bool blocked = false;
                    for (int32_t i = std::min(following.start.x, next.dest.x); i < std::max(following.start.x, next.dest.x) && !blocked; ++i)
                        for (int32_t j = std::min(following.start.y, next.dest.y); j < std::max(following.start.y, next.dest.y); ++j)
                            if (!accessible(room, ivec2{i, j})) {
                                blocked = true;
                                break;
                            }
                    if (!blocked)
                        con = next;
                }
            }
            bool const back = carry_backwards(state, c);
            uint32_t const lead = back ? 1 : 0;
            vec2 const target = room_view::middle_of_tile(con.dest);
            x.travel_dir = rw::lerp(x.travel_dir, custom::dir_vec(pos(state, c, lead), target), 0.4f);
            if (footing(state, c)) {
                for (uint32_t k = 0; k < 2; ++k) {
                    if (following.start.x == following.dest.x)
                        vel(state, c, k).x += mathf::min((target.x - pos(state, c, k).x) / 8.f, 1.2f);
                    else if (following.start.y == following.dest.y)
                        vel(state, c, k).y += mathf::min((target.y - pos(state, c, k).y) / 8.f, 1.2f);
                }
            }
            auto const& last = mv.last_followed;
            if (last.valid && (footing(state, c) || accessible(room, pos(state, c, lead)))
            && ((following.start.x != following.dest.x && last.start.x == last.dest.x) || (following.start.y != following.dest.y && last.start.y == last.dest.y))) {
                vel(state, c, lead) *= 0.7f;
                vel(state, c, 1 - lead) *= 0.5f;
            }
            // DropToFloor
            if (step.y < -1)
                x.footing_counter = 0;
            move_towards(state, c, room, target, rng);
            mv.last_followed = following;
        }

        // BigSpider.Swim
        void swim(sys::game_state& state, dcon::creature_id c, room_view const& room, pathing::field_id f, rw::rng& rng) {
            auto& x = pl(state, c);
            float const s0 = physics::submersion(state, room, chunk(state, c, 0));
            float const s1 = physics::submersion(state, room, chunk(state, c, 1));
            vel(state, c, 0) *= 1.f - 0.1f * s0;
            vel(state, c, 1) *= 1.f - 0.2f * s1;
            physics::set_go_through_floors(state, body(state, c), true);
            auto conn = follow_path(state, f, tile_of(pos(state, c, 0)));
            if (!conn.valid)
                conn = follow_path(state, f, tile_of(pos(state, c, 1)));
            ivec2 const at = here(state, c);
            if (!conn.valid && std::abs(at.y - room.default_water_level) < 4)
                conn = follow_path(state, f, ivec2{at.x, room.default_water_level});
            if (!conn.valid) {
                vel(state, c, 0).y += 0.5f;
                return;
            }
            // (the graphics' flip isn't turned from here)
            if (conn.start.y == conn.dest.y && conn.dest.y == room.default_water_level) {
                float const side = mathf::sign(room_view::middle_of_tile(conn.start).x - room_view::middle_of_tile(conn.dest).x);
                vel(state, c, 0).x -= side * 1.2f * s0;
                vel(state, c, 1).x += side * 0.5f * s1;
                x.footing_counter = 0;
                return;
            }
            vel(state, c, 0) *= 0.8f;
            vel(state, c, 0) += custom::dir_vec(pos(state, c, 0), room_view::middle_of_tile(conn.dest)) * 1.4f;
            x.footing_counter = int16_t(std::max(int32_t(x.footing_counter), 25));
            run(state, c, room, f, conn, rng);
            x.out_of_water_footing = 20;
        }

        //
        // BigSpiderAI
        //
        // BigSpiderAI.TileInEnclosedArea
        bool tile_in_enclosed_area(room_view const& room, ivec2 t) {
            int32_t n = 0;
            for (auto const d : custom::four_directions)
                if (room.get_tile(t + d * 2).solid() && ++n > 1)
                    return true;
            return false;
        }

        // BigSpiderAI.IdleScore (no Remix; AItile.visibility isn't kept:
        // its term is left out)
        float idle_score(hook_ctx& x, ivec2 coord) {
            auto& state = x.state;
            auto const& ai = ai_of(state, x.c);
            if (!x.room.inside(coord.x, coord.y) || !reachable(x, coord))
                return 100000.f;
            float num = 1.f;
            if (pathing::narrow_space(x.room, coord))
                num += 600.f;
            if (tile_in_enclosed_area(x.room, coord))
                num += 400.f;
            if (pathing::ai_terrain_proximity(state, x.room.id, coord) > 1)
                num += 200.f;
            num += tracker::threat_of_tile(tracker::of(state, x.c), coord, tracker_params().see_around_corners) * 500.f;
            for (uint32_t i = 0; i < ai.prev_idle_count; ++i) {
                float const d = rw::distance(rw::to_vec2(ai.prev_idle[i]), rw::to_vec2(coord));
                num += mathf::pow(mathf::inverse_lerp(80.f, 5.f, d), 2.f) * custom::lerp_map(float(i), 0.f, 8.f, 70.f, 15.f);
            }
            num += mathf::max(0.f, rw::distance(rw::to_vec2(here(state, x.c)), rw::to_vec2(coord)) - 40.f) / 20.f;
            return num - float(std::max(pathing::floor_altitude(x.room, coord), 6)) * 2.f;
        }

        // BigSpiderAI.TryAddReviveBuddy
        void try_add_revive_buddy(hook_ctx& hx, creature::tracked const& r) {
            auto& state = hx.state;
            auto const c = hx.c;
            auto& ai = ai_of(state, c);
            if (ai.revive_buddy || tracker::chance_of_finding(r, tracker_params().see_around_corners) < 0.05f)
                return;
            for (uint32_t i = 0; i < ai.other_spider_count; ++i) {
                auto const o = ai.other_spiders[i];
                if (is_big_spider(state, o) && creature::here(state, o, hx.room.id) && ai_of(state, o).revive_buddy == r.who)
                    return;
            }
            if (!can_be_revived(state, r.who, hx.room.id))
                return;
            ivec2 const g = tracker::best_guess(r);
            for (auto const d : custom::eight_directions_and_zero)
                if (reachable(hx, g + d)) {
                    ai.revive_buddy = r.who;
                    return;
                }
        }

        // the noises of this tick it hears (NoiseTracker: hearingSkill 1
        // sitting, else 0.3): BigSpiderAI.ReactToNoise (the legs' twitch is
        // the graphics')
        void hear_noises(sys::game_state& state, dcon::creature_id c, room_view const& room, rw::rng& rng) {
            auto& x = pl(state, c);
            auto& ai = ai_of(state, c);
            float const hearing = x.sitting ? 1.f : 0.3f;
            vec2 const main = pos(state, c, 0);
            for (auto const& n : state.noises(room.id)) {
                if (n.source == body(state, c) || !custom::dist_less(n.pos, main, n.strength * hearing))
                    continue;
                if (!x.sitting || ai.noise_reaction_delay > 0)
                    continue;
                pos(state, c, 1) += custom::rnv(rng) * 4.f;
                ai.noise_reaction_delay = rng.range(0, spitter(state, c) ? 100 : 10);
            }
        }

        // SpiderSpitModule.SameScreen etc. aren't ported: a spitter spits
        // between screens at the slugcat too (no camera in the simulation)

        // SpiderSpitModule.TargetChunkPos
        vec2 target_chunk_pos(sys::game_state const& state, spit_state const& sm) {
            uint32_t const n = creature::chunk_count_of(state, sm.spit_at);
            if (n == 0)
                return creature::main_pos(state, sm.spit_at);
            return state.world.body_chunk_get_pos(creature::chunk_of(state, sm.spit_at, uint32_t(sm.target_chunk) % n));
        }

        // SpiderSpitModule.SpitPosScore
        float spit_pos_score(hook_ctx& x, ivec2 test) {
            auto& state = x.state;
            auto const c = x.c;
            auto& sm = spit_of(state, c);
            auto const& ts = tracker::of(state, c);
            auto const* rep = tracker::find(ts, sm.spit_at);
            if (!rep)
                return std::numeric_limits<float>::max();
            ivec2 const crit = tracker::best_guess(*rep);
            vec2 const mt = room_view::middle_of_tile(test);
            if (!x.room.inside(test.x, test.y) || custom::dist_less(mt, sm.target_point, 220.f) || !reachable(x, test)
            || !x.room.visual_contact(mt, room_view::middle_of_tile(crit)))
                return std::numeric_limits<float>::max();
            ivec2 const at = here(state, c);
            auto const fdist = [](ivec2 a, ivec2 b) { return rw::distance(rw::to_vec2(a), rw::to_vec2(b)); };
            if (sm.random_crit_spit_delay > 0)
                return fdist(at, test);
            float num = std::fabs(20.f - fdist(crit, test)) * 2.f;
            num += mathf::clamp(fdist(sm.spit_pos, test), 10.f, 40.f);
            num += mathf::min(fdist(at, test), fdist(state.world.creature_get_destination(c), test)) * custom::lerp_map(float(rep->ticks_since_seen), 0.f, 100.f, 2.f, 0.5f);
            num += float(pathing::ai_terrain_proximity(state, x.room.id, test)) * 10.f;
            num += float(custom::int_clamp(crit.y + 5 - test.y, -10, 10)) * 2.f;
            vec2 const main = pos(state, c, 0);
            if (!custom::dist_less(main, mt, 900.f))
                num += 800.f;
            if (pathing::narrow_space(x.room, test))
                num += 1000.f;
            vec2 const behind = sm.target_point + custom::dir_vec(main, sm.target_point) * (rw::distance(main, sm.target_point) + 200.f);
            if (custom::dist_less(mt, behind, rw::distance(behind, sm.target_point) + 100.f))
                num += 900.f;
            float const my_dominance = state.world.big_spider_data_get_dominance(data(state, c));
            for (uint32_t i = 0; i < ts.count; ++i) {
                auto const& r = ts.reps[i];
                num -= mathf::min(20.f, fdist(test, tracker::best_guess(r)) * r.rel.intensity);
                // a more dominant spitter's spot is taken
                if (r.who != c && is_big_spider(state, r.who) && spitter(state, r.who) && creature::here(state, r.who, x.room.id)
                && state.world.big_spider_data_get_dominance(data(state, r.who)) > my_dominance)
                    num -= mathf::min(20.f, fdist(test, spit_of(state, r.who).spit_pos)) * 5.f;
            }
            return num;
        }

        // SpiderSpitModule.SitAndSpit
        bool sit_and_spit(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& sm = spit_of(state, c);
            if (!sm.go_to_spit_pos || sm.no_sit_delay >= 1 || behavior(state.world.creature_get_behavior(c)) != behavior::hunt)
                return false;
            vec2 const sp = room_view::middle_of_tile(sm.spit_pos);
            if (!custom::dist_less(pos(state, c, 0), sp, 80.f) || custom::dist_less(sp, sm.target_point, 300.f) || !sm.spit_at)
                return false;
            auto const* r = tracker::find(tracker::of(state, c), sm.spit_at);
            if (!r || !r->visual || !creature::here(state, sm.spit_at, room.id))
                return false;
            return (accessible(room, pos(state, c, 0)) || accessible(room, pos(state, c, 1))) && room.visual_contact(pos(state, c, 0), creature::main_pos(state, sm.spit_at));
        }
        // SpiderSpitModule.AbandonSitAndSpit
        bool abandon_sit_and_spit(sys::game_state& state, dcon::creature_id c) {
            auto& sm = spit_of(state, c);
            vec2 const sp = room_view::middle_of_tile(sm.spit_pos);
            if (!sm.go_to_spit_pos || !custom::dist_less(pos(state, c, 0), sp, 120.f) || !sm.spit_at)
                return true;
            auto const* r = tracker::find(tracker::of(state, c), sm.spit_at);
            return !r || r->ticks_since_seen > 20 || custom::dist_less(sp, sm.target_point, 220.f) || behavior(state.world.creature_get_behavior(c)) != behavior::hunt;
        }

        // SpiderSpitModule.Update
        void spit_update(hook_ctx& hx) {
            auto& state = hx.state;
            auto const c = hx.c;
            auto const& room = hx.room;
            auto& rng = hx.rng;
            auto& sm = spit_of(state, c);
            auto& ai = ai_of(state, c);
            auto const& ts = tracker::of(state, c);
            if (sm.tagged) {
                auto const* t = find_track(ai, sm.tagged);
                if (!t || t->tagged < 1)
                    sm.tagged = dcon::creature_id{};
            }
            if (sm.ammo < 4) {
                sm.ammo_regen += 1.f / (sm.fast_ammo_regen ? 60.f : 1200.f);
                if (sm.ammo_regen > 1.f) {
                    ++sm.ammo;
                    sm.ammo_regen -= 1.f;
                    if (sm.ammo > 3) {
                        sm.ammo_regen = 0.f;
                        sm.fast_ammo_regen = false;
                        ai.stay_away = false;
                    }
                }
            }
            vec2 const main = pos(state, c, 0);
            if (sm.random_crit_spit_delay < 1 && sm.ammo > 0 && rng.value() < 1.f / mathf::lerp(100.f, 2.f, tracker::threat_utility(ts)) && ts.count > 0) {
                auto const& r = ts.reps[uint32_t(rng.range(0, int32_t(ts.count)))];
                if (r.visual && creature::here(state, r.who, room.id) && consious(state, r.who) && r.rel.type != creature::rel::ignores
                && rng.value() < r.rel.intensity) {
                    auto const* t = find_track(ai, r.who);
                    vec2 const danger = creature::main_pos(state, r.who);
                    if ((!t || t->tagged < 1) && custom::dist_less(main, danger, 400.f + 200.f * r.rel.intensity) && !creature::grasped_object(state.world, c, 0)
                    && rw::dot(rw::normalized(pos(state, c, 1) - main), rw::normalized(main - danger)) > -0.3f) {
                        sm.spit_at = r.who;
                        sm.random_crit_spit_delay = 140;
                    }
                }
            }
            // a creature it no longer tracks is no target
            if (sm.spit_at && !tracker::find(ts, sm.spit_at))
                sm.spit_at = dcon::creature_id{};
            ivec2 const at = here(state, c);
            bool flag = false;
            if (sm.spit_at) {
                auto const* r = tracker::find(ts, sm.spit_at);
                auto const* t = find_track(ai, sm.spit_at);
                float const d = rw::distance(rw::to_vec2(sm.spit_pos), rw::to_vec2(at));
                flag = r->ticks_since_seen < (d < 4.f ? 100 : 600) && !(t && ready_for_spitter(*t))
                    && room.visual_contact(room_view::middle_of_tile(sm.spit_pos), room_view::middle_of_tile(tracker::best_guess(*r)));
            }
            if (flag != sm.go_to_spit_pos) {
                if (++sm.anti_flicker > 20)
                    sm.go_to_spit_pos = flag;
            } else {
                sm.anti_flicker = 0;
            }
            if (sm.random_crit_spit_delay > 0)
                --sm.random_crit_spit_delay;
            else if (auto const* p = tracker::prey(ts))
                sm.spit_at = p->who;
            if ((behavior(state.world.creature_get_behavior(c)) != behavior::hunt && sm.random_crit_spit_delay < 1) || !sm.spit_at || sm.ammo < 1) {
                sm.spit_pos = at;
                sm.target_trail_count = 0;
                sm.target_point = main;
                return;
            }
            auto const* r = tracker::find(ts, sm.spit_at);
            if (!r)
                return;
            if (rng.value() < 0.014285714f)
                sm.target_chunk = rng.range(0, 100);
            float num = spit_pos_score(hx, sm.spit_pos);
            int32_t const tries = sm.go_to_spit_pos ? 20 : 5;
            for (int32_t i = 0; i < tries; ++i) {
                vec2 p;
                if (rng.value() < 0.5f) {
                    vec2 const d = custom::rnv(rng);
                    float const a = rng.value();
                    p = main + d * a * rng.value() * 1700.f;
                } else {
                    vec2 const d = custom::rnv(rng);
                    p = room_view::middle_of_tile(sm.spit_pos) + d * rng.value() * 200.f;
                }
                ivec2 const t = tile_of(p);
                float const s = spit_pos_score(hx, t);
                if (s < num) {
                    sm.spit_pos = t;
                    num = s;
                }
            }
            if (sm.no_sit_delay > 0)
                --sm.no_sit_delay;
            bool const in_room = creature::here(state, sm.spit_at, room.id);
            if (r->visual && in_room)
                sm.target_point = target_chunk_pos(state, sm);
            else
                sm.target_point = room_view::middle_of_tile(tracker::best_guess(*r));
            if (r->ticks_since_seen < 40) {
                for (uint32_t i = std::min<uint32_t>(sm.target_trail_count, target_trail_max - 1); i > 0; --i)
                    sm.target_trail[i] = sm.target_trail[i - 1];
                sm.target_trail[0] = sm.target_point;
                sm.target_trail_count = uint8_t(std::min<uint32_t>(sm.target_trail_count + 1u, target_trail_max));
            } else {
                sm.target_trail_count = 0;
            }
            vec2 v = sm.target_point;
            if (sm.target_trail_count > 0 && sm.random_crit_spit_delay < 1)
                v = sm.target_trail[sm.target_trail_count - 1];
            v.y += custom::lerp_map(rw::distance(main, v), 70.f, 900.f, 0.f, 80.f, 2.f);
            sm.aim_dir = custom::dir_vec(main, v);
            auto const& x = pl(state, c);
            bool const sitting_spot = mv_of(state, c).has_spit_pos;
            if (can_spit(state, c, true) && r->visual && x.charging == 0.f
            && (sm.random_crit_spit_delay > 0
                || (custom::dist_less(main, sm.target_point, sitting_spot ? 1200.f : 100.f + float(sm.ammo) * 100.f)
                    && !custom::dist_less(main, sm.target_point, sitting_spot ? 0.f : 150.f) && sm.target_trail_count >= target_trail_max
                    && room.visual_contact(main, sm.target_trail[sm.target_trail_count - 1])))
            && room.visual_contact(main, sm.target_point))
                try_initiate_spit(state, c);
        }

        // BigSpiderAI.Update
        void ai_update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            hook_ctx hx{state, c, room, std::nullopt, rw::random(state.seed, state.tick, uint32_t(c.value))};
            auto& rng = hx.rng;
            tracker::hooks const h = make_hooks(hx);
            ivec2 const at = here(state, c);
            auto const& p = tracker_params();
            tracker::update(state, c, room, at, p, h);
            auto const& ts = tracker::of(state, c);
            auto& x = pl(state, c);
            auto& ai = ai_of(state, c);
            bool const sp = spitter(state, c);
            bool const mom = mother(state, c);
            if (sp)
                spit_update(hx);

            // otherSpiders, a dead one to revive
            if (ts.count > 0) {
                auto const& r = ts.reps[uint32_t(rng.range(0, int32_t(ts.count)))];
                if (r.who != c && is_big_spider(state, r.who) && creature::here(state, r.who, room.id)) {
                    bool known = false;
                    for (uint32_t i = 0; i < ai.other_spider_count; ++i)
                        known = known || ai.other_spiders[i] == r.who;
                    if (!creature::dead(state, r.who)) {
                        if (r.visual && !known && ai.other_spider_count < max_other_spiders)
                            ai.other_spiders[ai.other_spider_count++] = r.who;
                        // (the more dominant one's shyLightCycle: no light shyness)
                    } else if (!known && !sp) {
                        try_add_revive_buddy(hx, r);
                    }
                }
            }
            for (int32_t i = int32_t(ai.other_spider_count) - 1; i >= 0; --i) {
                auto const o = ai.other_spiders[i];
                if (!is_big_spider(state, o) || creature::dead(state, o) || !creature::here(state, o, room.id)) {
                    ai.other_spiders[i] = ai.other_spiders[ai.other_spider_count - 1];
                    --ai.other_spider_count;
                }
            }

            // UtilityComparer: threat (Lerp 0.5 up, Tick 0.005 down; the
            // spitter's weight from its random spit delay), prey (Lerp 0.2
            // up, Tick 0.01 down; weight 0.65, the big spider's by the
            // prey's distance), rain. (No StuckTracker: never GetUnstuck)
            float const threat = tracker::threat_utility(ts);
            ai.threat_smoothed = threat > ai.threat_smoothed ? mathf::lerp(ai.threat_smoothed, threat, 0.5f) : mathf::max(ai.threat_smoothed - 0.005f, threat);
            float const prey_raw = tracker::prey_utility(state, c, room, at, p);
            ai.prey_smoothed = prey_raw > ai.prey_smoothed ? mathf::lerp(ai.prey_smoothed, prey_raw, 0.2f) : mathf::max(ai.prey_smoothed - 0.01f, prey_raw);
            float threat_weight = 1.f;
            float prey_weight = 0.65f;
            auto const* prey = tracker::prey(ts);
            if (sp) {
                threat_weight = mathf::inverse_lerp(40.f, 10.f, float(spit_of(state, c).random_crit_spit_delay));
            } else if (prey) {
                prey_weight = custom::lerp_map(rw::distance(rw::to_vec2(at), rw::to_vec2(tracker::best_guess(*prey))), 26.f, 36.f, 1.f, 0.1f);
            }
            float const rain = dens::rain_utility(state, c, room.id);
            float utility = ai.threat_smoothed * threat_weight;
            behavior b = behavior::flee;
            if (ai.prey_smoothed * prey_weight > utility) {
                utility = ai.prey_smoothed * prey_weight;
                b = behavior::hunt;
            }
            if (rain > utility) {
                utility = rain;
                b = behavior::escape_rain;
            }
            if (utility < 0.05f)
                b = behavior::idle;
            if (ai.revive_buddy) {
                auto const t = ai.revive_buddy;
                auto const* r = tracker::find(ts, t);
                if (!r || tracker::chance_of_finding(*r, p.see_around_corners) < 0.05f || !can_be_revived(state, t, room.id) || !creature::dead(state, t)) {
                    ai.revive_buddy = dcon::creature_id{};
                } else if (utility < 0.3f) {
                    b = behavior::revive_buddy;
                    utility = 0.3f;
                }
            }
            if (b != behavior::flee) {
                auto const g = grasped(state, c);
                if (g && tracker::dynamic_relationship(state, c, g).type == creature::rel::eats) {
                    b = behavior::return_prey;
                    utility = 1.f;
                }
            }
            w.creature_set_behavior(c, uint8_t(b));
            w.creature_set_current_utility(c, utility);
            if (b != behavior::idle && b != behavior::get_unstuck)
                ai.idle_pos = at;
            if (x.sitting)
                x.run_speed = custom::lerp_and_tick(x.run_speed, 0.f, 0.01f, 0.016666668f);
            --ai.idle_pos_counter;

            ivec2& dest = w.creature_get_destination(c);
            switch (b) {
            case behavior::get_unstuck:
                x.run_speed = 1.f;
                break;
            case behavior::idle: {
                if (!x.sitting)
                    x.run_speed = custom::lerp_and_tick(x.run_speed, mom ? 0.15f : 0.5f, 0.01f, 0.016666668f);
                if (x.borrowed_time > 0) {
                    if (auto const e = dens::home_tile(state, c, room)) {
                        dest = *e;
                        break;
                    }
                }
                if (ai.idle_towards) {
                    dest = *ai.idle_towards;
                    if (rng.value() < 0.002f || custom::manhattan_distance(at, *ai.idle_towards) < 4)
                        ai.idle_towards = std::nullopt;
                    break;
                }
                // (leaving the room: AbstractBehavior, now and then, the way
                // out its destination: AbstractCreatureAI.WantToMigrate)
                if (ai.idle_pos_counter <= 0 && rng.value() < 0.05f)
                    migrate::abstract_behavior(state, c, room);
                if (auto const e = migrate::travel_tile(state, c, room); e && reachable(hx, *e)) {
                    dest = *e;
                    break;
                }
                ivec2 const coord{rng.range(0, room.width), rng.range(0, room.height)};
                if (idle_score(hx, coord) < idle_score(hx, ai.temp_idle_pos))
                    ai.temp_idle_pos = coord;
                dest = ai.idle_pos;
                if (custom::manhattan_distance(at, ai.idle_pos) < 3 && (pathing::narrow_space(room, at) || tile_in_enclosed_area(room, at)))
                    ai.idle_pos_counter -= 4;
                if (ai.idle_pos_counter < 1) {
                    ai.idle_pos_counter = rng.range(200, 800);
                    if (ai.prev_idle_count == max_prev_idle) {
                        for (uint32_t i = 1; i < max_prev_idle; ++i)
                            ai.prev_idle[i - 1] = ai.prev_idle[i];
                        --ai.prev_idle_count;
                    }
                    ai.prev_idle[ai.prev_idle_count++] = ai.idle_pos;
                    ai.idle_pos = ai.temp_idle_pos;
                    ai.temp_idle_pos = ivec2{rng.range(0, room.width), rng.range(0, room.height)};
                }
                break;
            }
            case behavior::hunt: {
                if (!x.sitting)
                    x.run_speed = custom::lerp_and_tick(x.run_speed, mom ? 0.15f : 1.f, 0.01f, 0.1f);
                if (!prey)
                    break;
                auto const t = prey->who;
                bool const there = creature::here(state, t, room.id);
                if (sp) {
                    auto const& sm = spit_of(state, c);
                    if (sm.go_to_spit_pos) {
                        dest = sm.spit_pos;
                        ai.idle_towards = sm.spit_pos;
                    } else {
                        dest = tracker::best_guess(*prey);
                        if (prey->visual && there && !consious(state, t) && custom::dist_less(pos(state, c, 0), creature::main_pos(state, t), 30.f))
                            vel(state, c, 0) += custom::dir_vec(pos(state, c, 0), creature::main_pos(state, t)) * 2.f;
                    }
                    break;
                }
                dest = tracker::best_guess(*prey);
                if (there && can_jump(state, c) && !x.jumping && x.charging == 0.f && footing(state, c)) {
                    vec2 const pp = creature::main_pos(state, t);
                    vec2 const c0 = pos(state, c, 0), c1 = pos(state, c, 1);
                    vec2 const behind = c1 - custom::dir_vec(c1, pp) * 30.f;
                    if (custom::dist_less(c0, pp, 120.f) && (accessible(room, behind) || room.get_tile(behind).solid()) && room.visual_contact(c0, pp)) {
                        if (rw::dot(rw::normalized(c0 - pp), rw::normalized(c1 - c0)) > 0.2f) {
                            initiate_jump(state, c, pp);
                        } else {
                            vel(state, c, 0) += custom::dir_vec(c0, pp);
                            vel(state, c, 1) -= custom::dir_vec(c0, pp);
                        }
                    }
                }
                break;
            }
            case behavior::flee:
                if (!x.sitting)
                    x.run_speed = custom::lerp_and_tick(x.run_speed, mom ? 0.2f : 1.f, 0.01f, 0.1f);
                dest = tracker::flee_to(state, c, room, at, 6, 30, p, h);
                break;
            case behavior::escape_rain:
            case behavior::return_prey:
                if (!x.sitting)
                    x.run_speed = custom::lerp_and_tick(x.run_speed, 1.f, 0.01f, 0.1f);
                if (auto const e = dens::home_tile(state, c, room))
                    dest = *e;
                break;
            case behavior::revive_buddy: {
                auto const t = ai.revive_buddy;
                auto const* r = tracker::find(ts, t);
                if (!r)
                    break;
                dest = tracker::best_guess(*r);
                auto const& mv = mv_of(state, c);
                if (creature::here(state, t, room.id) && !mv.reviving_buddy && custom::dist_less(pos(state, c, 0), creature::main_pos(state, t), 80.f)
                && room.visual_contact(pos(state, c, 0), creature::main_pos(state, t)))
                    vel(state, c, 0) += custom::dir_vec(pos(state, c, 0), creature::main_pos(state, t)) * 3.f;
                break;
            }
            }
            if (ai.noise_reaction_delay > 0)
                --ai.noise_reaction_delay;
            hear_noises(state, c, room, rng);
        }

        // BigSpiderAI.CollideWithKin
        void collide_with_kin(sys::game_state& state, dcon::creature_id c, dcon::creature_id other) {
            auto& w = state.world;
            ivec2 const theirs = w.creature_get_destination(other);
            if (custom::manhattan_distance(here(state, c), theirs) < 4)
                return;
            if ((w.big_spider_data_get_dominance(data(state, other)) > w.big_spider_data_get_dominance(data(state, c)) && !pl(state, other).sitting) || pl(state, c).sitting)
                ai_of(state, c).idle_towards = theirs;
        }

        // BigSpider.Act
        void act(sys::game_state& state, dcon::creature_id c, room_view const& room, rw::rng& rng) {
            auto& w = state.world;
            auto& x = pl(state, c);
            auto& mv = mv_of(state, c);
            auto const f = pathing::field_to(state, room.id, w.creature_get_destination(c), spider_movement);
            mv.jump_target_valid = false;
            if (physics::object_submersion(state, room, body(state, c)) > 0.3f) {
                swim(state, c, room, f, rng);
                ai_update(state, c, room);
                mv.has_spit_pos = false;
                return;
            }
            if (spitter(state, c)) {
                auto& sm = spit_of(state, c);
                if (mv.has_spit_pos) {
                    if (abandon_sit_and_spit(state, c)) {
                        mv.has_spit_pos = false;
                        sm.no_sit_delay = 60;
                    } else {
                        if (!custom::dist_less(mv.spit_dir, sm.aim_dir, 0.3f))
                            mv.spit_dir = rw::normalized(mv.spit_dir + sm.aim_dir * 0.2f);
                        float const dist = spitter(state, c) ? 25.f : 15.f;
                        vel(state, c, 0) *= 0.5f;
                        vel(state, c, 1) -= mv.spit_dir;
                        vec2 const a = mv.spit_pos + mv.spit_dir * mathf::lerp(5.f, -5.f, x.charging);
                        pos(state, c, 1) = rw::lerp(pos(state, c, 1), a - mv.spit_dir * dist * 0.5f, 0.2f);
                        vel(state, c, 0) *= 0.5f;
                        vel(state, c, 0) += mv.spit_dir;
                        pos(state, c, 0) = rw::lerp(pos(state, c, 0), a + mv.spit_dir * dist * 0.5f, 0.2f);
                        x.footing_counter = 30;
                    }
                    ai_update(state, c, room);
                } else if (sit_and_spit(state, c, room)) {
                    mv.has_spit_pos = true;
                    mv.spit_pos = pos(state, c, 0);
                    mv.spit_dir = custom::dir_vec(pos(state, c, 1), pos(state, c, 0));
                }
                if (x.charging > 0.f) {
                    // bracing against the recoil where a fall wouldn't be far
                    // (AItile.fallRiskTile: the floor below within 20 tiles)
                    if (pathing::floor_altitude(room, here(state, c)) < 20) {
                        vel(state, c, 1) -= sm.aim_dir * x.charging * 2.f;
                        vel(state, c, 0) += sm.aim_dir * x.charging;
                    }
                    mv.spit_dir = rw::normalized(mv.spit_dir + sm.aim_dir * 0.2f);
                    if (can_spit(state, c, false)) {
                        x.charging += 0.05f;
                        if (x.charging > 1.f)
                            spit(state, c, room);
                    } else {
                        x.charging = 0.f;
                    }
                }
                if (mv.has_spit_pos || x.charging > 0.f)
                    return;
            }
            if (x.jumping) {
                bool contact = false;
                for (uint32_t i = 0; i < 2; ++i) {
                    ivec2 const cp = w.body_chunk_get_contact_point(chunk(state, c, i));
                    if ((cp.x != 0 || cp.y != 0) && accessible(room, pos(state, c, i)))
                        contact = true;
                }
                if (contact)
                    ++x.footing_counter;
                else
                    x.footing_counter = 0;
                auto const* prey = tracker::prey(tracker::of(state, c));
                if (prey && creature::here(state, prey->who, room.id) && prey->visual) {
                    vec2 const pp = creature::main_pos(state, prey->who);
                    vel(state, c, 0) += custom::dir_vec(pos(state, c, 0), pp) * 1.f;
                    vel(state, c, 1) -= custom::dir_vec(pos(state, c, 0), pp) * 0.5f;
                    // the front legs reach for it (BigSpiderGraphics, from these)
                    mv.jump_target = pp;
                    mv.jump_target_valid = true;
                }
                if (footing(state, c))
                    x.jumping = 0;
                return;
            }
            // (no StuckTracker: its utility is 0, the shaking dies down)
            x.stuck_shake = custom::lerp_and_tick(x.stuck_shake, 0.f, 0.07f, 0.05f);
            if (x.stuck_shake > 0.f)
                for (uint32_t l = 0; l < 2; ++l) {
                    vel(state, c, l) += custom::rnv(rng) * rng.value() * 5.f * x.stuck_shake;
                    pos(state, c, l) += custom::rnv(rng) * rng.value() * 5.f * x.stuck_shake;
                }
            if (x.special_move_counter > 0) {
                --x.special_move_counter;
                x.footing_counter = int16_t(std::max(int32_t(x.footing_counter), 30));
                vec2 const to = room_view::middle_of_tile(mv.special_move_destination);
                move_towards(state, c, room, to, rng);
                x.travel_dir = rw::lerp(x.travel_dir, custom::dir_vec(pos(state, c, 0), to), 0.4f);
                if (custom::dist_less(pos(state, c, 0), to, 5.f))
                    x.special_move_counter = 0;
            } else {
                if (!accessible(room, pos(state, c, 0)) && !accessible(room, pos(state, c, 1)))
                    x.footing_counter = int16_t(custom::int_clamp(int32_t(x.footing_counter) - 3, 0, 35));
                ivec2 const dest = w.creature_get_destination(c);
                if (footing(state, c) && x.charging > 0.f) {
                    x.sitting = 1;
                    physics::set_go_through_floors(state, body(state, c), false);
                    x.charging += 0.05f;
                    vec2 const a = custom::dir_vec(pos(state, c, 0), mv.jump_at_pos);
                    vel(state, c, 0) += a * mathf::pow(x.charging, 2.f);
                    vel(state, c, 1) -= a * mathf::lerp(0.7f, 2.f, x.charging);
                    if (x.charging >= 1.f)
                        attack(state, c, room, rng);
                } else if ((tile_of(pos(state, c, 0)) == dest || tile_of(pos(state, c, 1)) == dest) && tracker::threat_utility(tracker::of(state, c)) < 0.5f) {
                    x.sitting = 1;
                    physics::set_go_through_floors(state, body(state, c), false);
                } else {
                    bool const back = carry_backwards(state, c);
                    auto conn = follow_path(state, f, tile_of(pos(state, c, back ? 1 : 0)));
                    if (!conn.valid)
                        conn = follow_path(state, f, tile_of(pos(state, c, back ? 0 : 1)));
                    if (conn.valid)
                        run(state, c, room, f, conn, rng);
                    else
                        physics::set_go_through_floors(state, body(state, c), false);
                }
            }
            ai_update(state, c, room);
            auto const head = chunk(state, c, 0);
            if (consious(state, c) && !custom::dist_less(w.body_chunk_get_pos(head), w.body_chunk_get_last_pos(head), 2.f))
                x.run_cycle += 0.0625f;
        }

        // BigSpider.CarryObject
        void carry_object(sys::game_state& state, dcon::creature_id c, room_view const& room, rw::rng& rng) {
            auto& w = state.world;
            auto& mv = mv_of(state, c);
            auto const t = grasped(state, c);
            uint32_t const k = creature::grasp_chunk(w, c, 0);
            if (!t || !creature::here(state, t, room.id) || k >= creature::chunk_count_of(state, t)) {
                lose_all_grasps(state, c);
                return;
            }
            if (rng.value() < 0.025f && tracker::dynamic_relationship(state, c, t).type != creature::rel::eats) {
                lose_all_grasps(state, c);
                return;
            }
            float const my_mass = mass(state, c);
            mv.carry_object_mass = creature::mass_of(state, t);
            if (mv.carry_object_mass <= my_mass * 1.4f)
                mv.carry_object_mass /= 2.f;
            else if (mv.carry_object_mass <= my_mass / 5.f)
                mv.carry_object_mass = 0.f;
            auto const main = chunk(state, c, 0);
            auto const gc = creature::chunk_of(state, t, k);
            float const num = w.body_chunk_get_rad(main) + w.body_chunk_get_rad(gc);
            vec2& mp = w.body_chunk_get_pos(main);
            vec2& gp = w.body_chunk_get_pos(gc);
            vec2 const a = -custom::dir_vec(mp, gp) * (num - rw::distance(mp, gp));
            float num2 = w.body_chunk_get_mass(gc) / (w.body_chunk_get_mass(main) + w.body_chunk_get_mass(gc));
            num2 *= 0.2f;
            mp += a * num2;
            w.body_chunk_get_vel(main) += a * num2;
            gp -= a * (1.f - num2);
            w.body_chunk_get_vel(gc) -= a * (1.f - num2);
            vec2 const hold = mp + custom::dir_vec(pos(state, c, 1), mp) * num;
            vec2 const rel = w.body_chunk_get_vel(gc) - w.body_chunk_get_vel(main);
            w.body_chunk_set_vel(gc, w.body_chunk_get_vel(main));
            if (w.creature_get_entering_shortcut(c).x < 0
            && (rw::magnitude(rel) * w.body_chunk_get_mass(gc) > 30.f || !custom::dist_less(hold, gp, 70.f + w.body_chunk_get_rad(gc)))) {
                lose_all_grasps(state, c);
                return;
            }
            // BodyChunk.MoveFromOutsideMyUpdate
            gp = hold;
            // PhysicalObject.PushOutOf: its other chunks out of the spider
            uint32_t const n = creature::chunk_count_of(state, t);
            for (uint32_t i = 0; i < 2; ++i) {
                vec2 const bp = pos(state, c, i);
                float const rad = w.body_chunk_get_rad(chunk(state, c, i));
                for (uint32_t j = 0; j < n; ++j) {
                    if (j == k)
                        continue;
                    auto const other = creature::chunk_of(state, t, j);
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
        }

        // Creature.Violence (HealthState) with BigSpider.Stun
        void base_violence(sys::game_state& state, dcon::creature_id c, uint32_t k, vec2 momentum, float damage, float stun_bonus) {
            auto& w = state.world;
            auto rng = rw::random(state.seed, state.tick, rw::ids(uint32_t(c.value), k));
            auto const& t = tp(state, c);
            auto const hit = chunk(state, c, k < 2 ? k : 0);
            w.body_chunk_get_vel(hit) += rw::clamp_magnitude(momentum / w.body_chunk_get_mass(hit), 10.f);
            float const dmg = damage / t.base_damage_resistance;
            float st = (damage * 30.f + stun_bonus) / t.base_stun_resistance;
            float& health = w.creature_get_health(c);
            st *= 1.5f + mathf::inverse_lerp(0.5f, 0.f, health) * rng.value();
            stun(state, c, int32_t(st), rng);
            health -= dmg;
            if (rng.value() < -health || health < -1.f || (health < 0.f && rng.value() < 0.33f) || dmg >= t.instant_death_damage_limit)
                creature::die(state, c);
        }

        // BigSpider.BabyPuff: the mother bursts, her young swarm out (made
        // after the rooms' update; wherever she dies, camera or not)
        void baby_puff(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& mv = mv_of(state, c);
            if (!mother(state, c) || mv.spew_babies)
                return;
            mv.spew_babies = true;
            vec2 const at = pos(state, c, 0);
            if (state.viewed(room.id)) {
                // SporeCloud (nonToxic), PuffBallSkin (cosmetic)
                auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
                for (int i = 0; i < 70; ++i)
                    particles::spawn(state, room.id, particles::kind::dust, at, custom::rnv(rng) * rng.value() * 10.f, 14.f, 2.f, vec3{0.1f, 0.25f, 0.1f}, 0.8f);
                for (int i = 0; i < 7; ++i)
                    particles::spawn(state, room.id, particles::kind::spark, at, custom::rnv(rng) * rng.value() * 16.f, 3.f, 1.f, vec3{0.1f, 0.3f, 0.1f});
            }
            // (no Puffball_Eplode sound id)
            state.defer(room.id, [at, rid = room.id](sys::game_state& s) {
                for (int i = 0; i < 25; ++i)
                    spawn_young(s, rid, at);
            });
        }
    }

    char const* to_string(variant v) noexcept {
        switch (v) {
        case variant::big: return "big spider";
        case variant::spitter: return "spitter spider";
        case variant::mother: return "mother spider";
        }
        return "big spider";
    }

    bool footing(sys::game_state const& state, dcon::creature_id c) noexcept {
        auto const& x = pl(state, c);
        return x.footing_counter > 20 || x.out_of_water_footing > 0;
    }

    dcon::creature_id grasped(sys::game_state const& state, dcon::creature_id c) noexcept {
        auto const t = creature::grasped(state.world, c, 0);
        return creature::live(state, t) ? t : dcon::creature_id{};
    }

    vec3 yellow_col(sys::game_state const& state, dcon::creature_id c) noexcept {
        return state.world.big_spider_data_get_yellow_col(data(state, c));
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 at, variant v) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::big_spider);
        auto const d = data(state, c);
        auto rng = rw::random(state.seed, 0, uint32_t(c.value));
        int32_t seed = int32_t(rng.next() & 0x7FFFFFFFu);
        if (state.spawn_seed >= 0)
            seed = state.spawn_seed;
        bool const sp = v == variant::spitter;
        bool const mom = v == variant::mother;
        physics::object_desc desc;
        desc.air_friction = 0.999f;
        desc.gravity = 0.9f;
        desc.bounce = 0.1f;
        desc.surface_friction = 0.4f;
        desc.water_friction = 0.96f;
        desc.buoyancy = 0.95f;
        auto const o = physics::create_object(state, room, desc);
        // never inside terrain
        auto const view = world::get_room_view(state, room);
        ivec2 const t0 = tile_of(at);
        if (view.get_tile(t0).solid()) {
            int best = 1 << 30;
            for (int dy = -6; dy <= 6; ++dy)
                for (int dx = -6; dx <= 6; ++dx) {
                    ivec2 const t = t0 + ivec2{dx, dy};
                    if (view.inside(t.x, t.y) && !view.get_tile(t).solid() && dx * dx + dy * dy < best) {
                        best = dx * dx + dy * dy;
                        at = room_view::middle_of_tile(t);
                    }
                }
        }
        // two chunks: the head (5, a third of the mass) and the body (9, two
        // thirds), 15 apart (the spitter 25)
        float const m = mom ? 2.f : sp ? 1.2f : 0.8f;
        auto const c0 = physics::add_chunk(state, o, at, 5.f, m * 0.33333334f);
        auto const c1 = physics::add_chunk(state, o, at + vec2{0.f, -1.f}, 9.f, m * 0.6666667f);
        physics::add_connection(state, o, c0, c1, sp ? 25.f : 15.f, physics::connection_type::normal, 1.f, 0.5f);
        w.try_create_creature_body(c, o);
        auto& x = pl(state, c);
        x.variant = v;
        x.charging = 0.f;
        x.mandibles_charged = 0.f;
        x.jump_stamina = 1.f;
        x.run_speed = 0.f;
        x.run_cycle = 0.f;
        x.stuck_shake = 0.f;
        x.death_convulsions = 1.f;
        x.travel_dir = vec2{0.f, 0.f};
        x.footing_counter = 0;
        x.out_of_water_footing = 0;
        x.special_move_counter = 0;
        x.can_bite = 0;
        x.can_cling = 0;
        x.grabbed_counter = 0;
        x.borrowed_time = -1;
        x.jumping = 0;
        x.sitting = 0;
        x.off_ground = 0;
        // yellowCol, rolled from the ID's seed
        {
            rw::unity_random r{seed};
            vec3 const base = sp ? vec3{1.f, 0.f, 0.f} : mom ? vec3{0.f, 1.f, 0.f} : vec3{1.f, 0.8f, 0.3f};
            float const h = r.value(), s = r.value(), l = r.value();
            vec3 const other = custom::hsl2rgb(h, s, l);
            float const t = mathf::clamp01(r.value() * 0.2f);
            w.big_spider_data_set_yellow_col(d, base + (other - base) * t);
        }
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_set_grasps(c, creature::grasp_pair{});
        w.creature_set_behavior(c, uint8_t(behavior::idle));
        w.creature_set_current_utility(c, 0.f);
        w.creature_set_destination(c, tile_of(at));
        w.big_spider_data_set_seed(d, seed);
        // AbstractCreature.personality.dominance
        w.big_spider_data_set_dominance(d, rng.value());
        w.big_spider_data_set_move(d, move_state{});
        ai_state ai;
        ai.idle_pos = tile_of(at);
        ai.temp_idle_pos = tile_of(at);
        w.big_spider_data_set_ai(d, ai);
        spit_state sm;
        // SpiderSpitModule: targetChunk = Random.Range(0, 100)
        sm.target_chunk = rng.range(0, 100);
        sm.spit_pos = tile_of(at);
        w.big_spider_data_set_spit(d, sm);
        tracker::clear(state, c);
        big_spider_graphics::init(state, c);
        big_spider_graphics::reset_positions(state, c);
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        lose_all_grasps(state, c);
        creature::release_target(state, c);
        creature::erase(state, c);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        auto const o = body(state, c);
        auto& x = pl(state, c);
        bool const sp = spitter(state, c);
        // HealthState: hurt, it may die or faint, else heals
        {
            float& health = w.creature_get_health(c);
            if (!w.creature_get_dead(c) && health < 1.f) {
                if (health < 0.f && rng.value() < -health && rng.value() < 0.025f)
                    creature::die(state, c);
                if (!w.creature_get_dead(c) && rng.value() * 0.7f > health && rng.value() < 0.125f)
                    stun(state, c, rng.range(1, rng.range(1, 27 - custom::int_clamp(int32_t(20.f * health), 0, 10))), rng);
            }
            if (!w.creature_is_valid(c))
                return;
        }
        if (x.out_of_water_footing > 0)
            --x.out_of_water_footing;
        if (x.can_bite > 0) {
            bool contact = false;
            for (uint32_t i = 0; i < 2; ++i) {
                ivec2 const cp = w.body_chunk_get_contact_point(chunk(state, c, i));
                if (cp.x != 0 || cp.y != 0)
                    contact = true;
            }
            if (contact) {
                if (x.off_ground)
                    x.can_bite = int16_t(std::min<int32_t>(x.can_bite, 10));
                x.off_ground = 0;
            } else {
                x.off_ground = 1;
            }
            --x.can_bite;
        }
        auto& mv0 = mv_of(state, c);
        mv0.bounce_sound_vol = mathf::min(1.f, mv0.bounce_sound_vol + 0.008333334f);
        {
            float& health = w.creature_get_health(c);
            if (health > 0.f)
                health = mathf::min(1.f, health + 0.0014705883f);
        }
        // Creature.Update + PhysicalObject.Update
        creature::stun_countdown(state, c);
        physics::update_object(state, room, o, nullptr, nullptr);
        if (!w.creature_is_valid(c))
            return;

        // grabbed: after a moment it bites what holds it
        uint32_t holder_grasp = 0;
        auto const holder = creature::holder(state, c, &holder_grasp);
        if (!w.creature_get_dead(c) && w.creature_get_stun(c) < 35 && holder && w.creature_get_type(holder) != creature::type::vulture
        && w.creature_get_type(holder) != creature::type::leech) {
            ++x.grabbed_counter;
            if (x.grabbed_counter == 25 && rng.value() < 0.65f * w.creature_get_health(c)) {
                vec2 const main = pos(state, c, 0);
                uint32_t const n = creature::chunk_count_of(state, holder);
                uint32_t best = creature::main_chunk_of(state, holder);
                float dst = std::numeric_limits<float>::max();
                for (uint32_t j = 0; j < n; ++j) {
                    vec2 const hp = w.body_chunk_get_pos(creature::chunk_of(state, holder, j));
                    if (custom::dist_less(main, hp, dst)) {
                        dst = rw::distance(main, hp);
                        best = j;
                    }
                }
                vec2 const hp = w.body_chunk_get_pos(creature::chunk_of(state, holder, best));
                if (auto* t = find_track(ai_of(state, c), holder))
                    t->accustomed = 0;
                if (sp) {
                    spit_of(state, c).spit_at = holder;
                    spit_of(state, c).random_crit_spit_delay = 140;
                }
                creature::violence_on(state, holder, best, custom::dir_vec(main, hp) * 8.f, sp ? 0.6f : 0.4f, 20.f, creature::damage_type::bite,
                    creature::template_of(state, c));
                vel(state, c, 0) += custom::dir_vec(hp, main) * 8.f;
                ai_of(state, c).stay_away = true;
                // (no Big_Spider_Slash_Creature sound)
                if (!creature::held(state, c))
                    w.creature_set_stun(c, 0);
            } else if (x.grabbed_counter < 25) {
                for (uint32_t i = 0; i < 2; ++i) {
                    pos(state, c, i) += custom::rnv(rng) * rng.value() * 6.f;
                    vel(state, c, i) += custom::rnv(rng) * rng.value() * 6.f;
                }
            }
        } else {
            x.grabbed_counter = 0;
        }
        // (vanilla's legs, gripping where a camera looks, pull the body
        // back towards their grips off the accessible tiles: not here)

        x.sitting = 0;
        if (x.borrowed_time > 0)
            --x.borrowed_time;
        else if (x.borrowed_time == 0 && !w.creature_get_dead(c))
            w.creature_get_health(c) -= 0.007142857f;

        auto& mv = mv_of(state, c);
        if (consious(state, c)) {
            if ((accessible(room, pos(state, c, 0)) || accessible(room, pos(state, c, 1))) && x.footing_counter < 30000)
                ++x.footing_counter;
            act(state, c, room, rng);
            if (!w.creature_is_valid(c))
                return;
            if (x.charging == 0.f && x.can_bite == 0) {
                x.mandibles_charged = mathf::max(0.f, x.mandibles_charged - 0.1f);
                if (!x.jumping) {
                    x.jump_stamina = mathf::min(1.f, x.jump_stamina + 0.0033333334f);
                    if (x.jump_stamina == 1.f && !sp)
                        ai_of(state, c).stay_away = false;
                }
            } else {
                x.mandibles_charged = custom::lerp_and_tick(x.mandibles_charged, (x.charging > 0.f || x.can_bite > 0) ? 1.f : 0.f, 0.01f, 0.022727273f);
            }
            if (ai_of(state, c).threat_smoothed < 0.9f)
                x.can_cling = 40;
            else if (x.can_cling > 0)
                --x.can_cling;
            // breathing life into a dead kin
            if (auto const b = mv.reviving_buddy) {
                if (!can_be_revived(state, b, room.id) || !custom::dist_less(creature::main_pos(state, b), pos(state, c, 0), 50.f) || !w.creature_get_dead(b)
                || w.creature_get_entering_shortcut(b).x >= 0) {
                    mv.reviving_buddy = dcon::creature_id{};
                    if (rng.value() < 0.5f)
                        release_all_grab_chunks(state, c);
                } else {
                    vec2& bp = pos(state, b, 0);
                    vec2 const a2 = custom::dir_vec(bp, pos(state, c, 0)) * (rw::distance(bp, pos(state, c, 0)) - 20.f);
                    vel(state, b, 0) += a2 * 0.5f;
                    bp += a2 * 0.5f;
                    vel(state, c, 0) -= a2 * 0.5f;
                    pos(state, c, 0) -= a2 * 0.5f;
                    vel(state, c, 1) += custom::rnv(rng) * 4.f;
                    pos(state, c, 1) += custom::rnv(rng) * 4.f;
                    // (no Big_Spider_Revive sound) WaterDrip
                    if (state.viewed(room.id)) {
                        auto gr = rw::random(state.seed, state.tick, uint32_t(c.value));
                        particles::spawn(state, room.id, particles::kind::drop, rw::lerp(pos(state, c, 0), bp, gr.value()),
                            vel(state, c, 0) * gr.value() + custom::rnv(gr) * gr.value() * 7.f, 1.5f, 0.75f, vec3{0.6f, 0.6f, 0.65f});
                    }
                    // WeightedPush(1, 0, dir, 3)
                    vec2 const push = custom::dir_vec(bp, pos(state, c, 0)) * 3.f;
                    vel(state, c, 1) += push * 0.5f;
                    vel(state, c, 0) -= push * 0.5f;
                    auto& bx = pl(state, b);
                    bx.borrowed_time = int16_t(std::min<int32_t>(int32_t(bx.borrowed_time) + 5, 30000));
                    float& bh = w.creature_get_health(b);
                    bh = mathf::min(1.f, bh + (spitter(state, b) ? 0.5f : 1.f) / (bh < -1.f ? 160.f : 60.f));
                    if (bx.borrowed_time > 300 && bh == 1.f)
                        revive(state, b);
                }
            }
        } else {
            mv.has_spit_pos = false;
            x.footing_counter = 0;
            x.charging = 0.f;
            x.jumping = 0;
            x.mandibles_charged *= 0.8f;
            mv.reviving_buddy = dcon::creature_id{};
            if (x.death_convulsions > 0.f) {
                if (w.creature_get_dead(c))
                    x.death_convulsions = mathf::max(0.f, x.death_convulsions - rng.value() / 80.f);
                float const dc = mathf::pow(x.death_convulsions, 0.5f);
                ivec2 const cp0 = w.body_chunk_get_contact_point(chunk(state, c, 0));
                if (cp0.x != 0 || cp0.y != 0)
                    vel(state, c, 0) += custom::rnv(rng) * rng.value() * 8.f * dc;
                ivec2 const cp1 = w.body_chunk_get_contact_point(chunk(state, c, 1));
                if (cp1.x != 0 || cp1.y != 0)
                    vel(state, c, 1) += custom::rnv(rng) * rng.value() * 4.f * dc;
                // (no Big_Spider_Death_Rustle / Take_Damage sounds)
            }
            lose_all_grasps(state, c);
        }
        if (footing(state, c)) {
            float const g = w.physical_object_get_g(o);
            for (uint32_t i = 0; i < 2; ++i) {
                vel(state, c, i) *= 0.8f;
                vel(state, c, i).y += g;
            }
        }

        // the legs holding on to what they grabbed
        int32_t num = 0;
        float const main_mass = w.body_chunk_get_mass(chunk(state, c, 0));
        bool const grabby = legs_grabby(state, c);
        for (uint32_t s = 0; s < 2; ++s)
            for (uint32_t l = 0; l < 4; ++l) {
                auto& g = mv.grab[s][l];
                if (!g.who)
                    continue;
                if (!creature::here(state, g.who, room.id) || g.chunk >= creature::chunk_count_of(state, g.who)) {
                    g = grab_chunk{};
                    continue;
                }
                auto const k = creature::chunk_of(state, g.who, g.chunk);
                vec2& kp = w.body_chunk_get_pos(k);
                float const rad = w.body_chunk_get_rad(k);
                // (Player.GraspWiggle isn't read: a slugcat struggles like an
                // average one)
                float const odds = (creature::is_slugcat(w, g.who) ? 105.f : 160.f) * (grabby ? 1.f : 0.2f);
                if (!custom::dist_less(kp, pos(state, c, 0), 60.f + rad) || rng.value() < 1.f / odds || creature::grasped_object(w, c, 0) || x.charging > 0.f
                || x.can_bite > 0 || w.creature_get_entering_shortcut(g.who).x >= 0 || !room.visual_contact(pos(state, c, 0), kp)) {
                    g = grab_chunk{};
                } else if (!custom::dist_less(kp, pos(state, c, 0), 25.f + rad)) {
                    vec2 const a3 = custom::dir_vec(kp, pos(state, c, 0)) * (rw::distance(kp, pos(state, c, 0)) - (25.f + rad));
                    float num4 = main_mass / (main_mass + w.body_chunk_get_mass(k));
                    w.body_chunk_get_vel(k) += a3 * num4 * 0.65f;
                    kp += a3 * num4 * 0.65f;
                    vel(state, c, 0) -= a3 * (1.f - num4) * 0.65f;
                    pos(state, c, 0) -= a3 * (1.f - num4) * 0.65f;
                    ++num;
                    num4 = main_mass / (main_mass + w.body_chunk_get_mass(chunk(state, c, 1)));
                    vec2 const d = custom::dir_vec(kp, pos(state, c, 0));
                    vel(state, c, 1) += d * num4 * 0.4f;
                    vel(state, c, 0) -= d * (1.f - num4) * 0.4f;
                    if ((rng.value() < 0.33333334f && grabby) || mv.reviving_buddy) {
                        int32_t const nx = rng.range(0, 2);
                        int32_t const ny = rng.range(0, rng.range(0, 4));
                        uint32_t const n = creature::chunk_count_of(state, g.who);
                        uint32_t const kj = uint32_t(rng.range(0, int32_t(n)));
                        auto const k3 = creature::chunk_of(state, g.who, kj);
                        if (!mv.grab[nx][ny].who && custom::dist_less(w.body_chunk_get_pos(k3), pos(state, c, 0), 50.f + w.body_chunk_get_rad(k3)))
                            mv.grab[nx][ny] = grab_chunk{g.who, uint8_t(kj)};
                    }
                }
            }
        if (num > 3 && physics::object_submersion(state, room, o) == 0.f)
            x.footing_counter = 0;
        // the spitter's legs pass what they hold on to its jaws
        if (sp && num > 3 && !creature::grasped_object(w, c, 0)) {
            vel(state, c, 1) += custom::rnv(rng) * rng.value() * 5.f;
            bool done = false;
            for (uint32_t s = 0; s < 2 && !done; ++s)
                for (uint32_t l = 0; l < 4; ++l) {
                    auto const g = mv.grab[s][l];
                    if (!g.who || !creature::here(state, g.who, room.id))
                        continue;
                    bool const awake = consious(state, g.who);
                    if (rng.value() < 1.f / (awake ? 120.f : 8.f) && tracker::dynamic_relationship(state, c, g.who).type == creature::rel::eats) {
                        if (rng.value() < 0.125f || !awake) {
                            creature::grab(state, c, 0, g.who, g.chunk);
                            // (no Big_Spider_Grab_Creature sound)
                        } else {
                            creature::stun(state, g.who, 20);
                        }
                        vec2 const kp = w.body_chunk_get_pos(creature::chunk_of(state, g.who, g.chunk));
                        vel(state, c, 0) += custom::dir_vec(pos(state, c, 0), kp) * 11.f;
                        pos(state, c, 0) += custom::dir_vec(pos(state, c, 0), kp) * 6.f;
                        done = true;
                        break;
                    }
                }
        }
        x.travel_dir *= x.sitting ? 0.5f : 0.9995f;
        // scrambling in the air, fleeing
        if (consious(state, c) && !footing(state, c) && behavior(w.creature_get_behavior(c)) == behavior::flee)
            for (uint32_t i = 0; i < 2; ++i)
                if (accessible(room, pos(state, c, i)))
                    vel(state, c, i) += custom::deg_to_vec(rng.value() * 360.f) * rng.value() * 5.f;
        if (creature::grasped_object(w, c, 0)) {
            carry_object(state, c, room, rng);
            return;
        }
        mv.carry_object_mass = 0.f;
    }

    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) {
        // what it holds is kept out of it by CarryObject
        return !creature::holds(state.world, c, t);
    }

    void collide(sys::game_state& state, dcon::creature_id c, room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j) {
        auto& w = state.world;
        if (!consious(state, c))
            return;
        auto& x = pl(state, c);
        auto& mv = mv_of(state, c);
        auto& ai = ai_of(state, c);
        bool const sp = spitter(state, c);
        auto rng = rw::random(state.seed, state.tick, rw::ids(uint32_t(c.value), uint32_t(t.value) * 4u + i * 2u + j));
        {
            hook_ctx hx{state, c, room, std::nullopt, rw::random(state.seed, state.tick, uint32_t(c.value) ^ 0x5bd1e995u)};
            tracker::see(state, c, room, t, make_hooks(hx));
        }
        auto const& ts = tracker::of(state, c);
        if (tracker::find(ts, t))
            track_of(ai, ts, t).accustomed += 10;
        bool done = false;
        bool const awake = consious(state, t);
        auto const me = creature::template_of(state, c);
        auto const them = creature::template_of(state, t);
        auto const tk = creature::chunk_of(state, t, j);
        vec2 const tkp = w.body_chunk_get_pos(tk);
        vec2 const main = pos(state, c, 0);
        if (i == 0 && !creature::grasped_object(w, c, 0)) {
            bool const facing = rw::dot(custom::dir_vec(pos(state, c, 1), main), custom::dir_vec(main, tkp)) > 0.f;
            bool const bite = ((!sp && x.mandibles_charged > 0.8f && x.can_bite > 0) || (sp && rng.value() < 0.25f && !awake)) && facing
                && tracker::dynamic_relationship(state, c, t).type == creature::rel::eats;
            if (bite) {
                if (state.viewed(room.id))
                    for (int q = 0; q < 4; ++q)
                        particles::spawn(state, room.id, particles::kind::drop, rw::lerp(main, tkp, rng.value()), custom::rnv(rng) * rng.value() * 14.f, 1.5f, 0.75f,
                            vec3{0.6f, 0.6f, 0.65f});
                if (rng.value() < custom::lerp_map(creature::mass_of(state, t), 0.84f, sp ? 5.5f : 3.f, 0.5f, 0.15f, 0.12f) || !awake) {
                    creature::grab(state, c, 0, t, j);
                    done = true;
                }
                // (no Big_Spider_Grab_Creature / Slash_Creature sounds)
                x.can_bite = 0;
                float const dmg = sp ? 0.6f : (rng.value() < 0.5f ? 1.2f : 0.4f);
                creature::violence_on(state, t, j, custom::dir_vec(main, tkp) * (sp ? 8.f : 6.f), dmg, sp ? 20.f : 60.f, creature::damage_type::bite, me);
                release_all_grab_chunks(state, c);
                if (!w.creature_is_valid(c) || !w.creature_is_valid(t))
                    return;
            } else if (creature::static_relationship(me, them).type == creature::rel::eats
                       && (creature::static_relationship(them, me).type != creature::rel::eats || rng.value() < 0.1f) && legs_grabby(state, c)) {
                int32_t const gx = rng.range(0, 2);
                int32_t const gy = rng.range(0, rng.range(2, 4));
                mv.grab[gx][gy] = grab_chunk{t, uint8_t(j)};
                done = true;
            }
        }
        // fending off what comes at the kin it revives
        if (!done && mv.reviving_buddy && x.jump_stamina > 0.2f) {
            auto const rt = tracker::dynamic_relationship(state, c, t).type;
            if (rt == creature::rel::eats || rt == creature::rel::afraid) {
                vec2 const mid = (pos(state, c, 0) + pos(state, c, 1)) / 2.f;
                vec2 const d = custom::dir_vec(mid, tkp);
                float const dist = sp ? 25.f : 15.f;
                pos(state, c, 0) = mid + d * dist * 0.5f;
                pos(state, c, 1) = mid - d * dist * 0.5f;
                jump(state, c, d, 0.8f);
                x.can_bite = 40;
                x.mandibles_charged = 1.f;
                release_all_grab_chunks(state, c);
                mv.reviving_buddy = dcon::creature_id{};
                x.jump_stamina = mathf::max(0.f, x.jump_stamina - 0.5f);
                done = true;
            }
        }
        // bumped into something big while it can't bite: away
        if (!done && (sp || x.mandibles_charged <= 0.8f || x.can_bite <= 0) && (sp || x.jump_stamina > 0.15f) && !creature::grasped_object(w, c, 0) && awake
        && creature::static_relationship(them, me).intensity > 0.f && creature::mass_of(state, t) > mass(state, c) * 0.2f) {
            for (auto const& side : mv.grab)
                for (auto const& g : side)
                    if (g.who == t)
                        return;
            float const tm = w.body_chunk_get_mass(tk);
            if (sp) {
                vec2 const d = custom::dir_vec(tkp, main);
                vel(state, c, 0) += d * 8.f;
                vel(state, c, 1) += d * 8.f;
                w.body_chunk_get_vel(tk) -= rw::clamp_magnitude(d * 8.f * mass(state, c) / tm, 15.f);
                mv.bounce_sound_vol = mathf::max(0.f, mv.bounce_sound_vol - 0.2f);
            } else {
                vec2 const d = custom::dir_vec(tkp, main + vec2{0.f, 10.f});
                jump(state, c, d, 0.1f + 0.4f * mv.bounce_sound_vol);
                mv.bounce_sound_vol = mathf::max(0.f, mv.bounce_sound_vol - 0.2f);
                w.body_chunk_get_vel(tk) -= rw::clamp_magnitude(d * 8.f * mass(state, c) / tm, 15.f);
                pos(state, c, 0).y += 20.f;
                pos(state, c, 1).y += 10.f;
                x.jump_stamina = mathf::max(0.f, x.jump_stamina - 0.15f);
            }
            if (awake && creature::static_relationship(them, me).type == creature::rel::eats)
                ai.stay_away = true;
        }
    }

    void collide_kin(sys::game_state& state, dcon::creature_id c, uint32_t i, dcon::creature_id other, uint32_t j) {
        auto& w = state.world;
        auto const room = creature::room_of(state, c);
        // both sides' BigSpider.Collide (each pair meets once here)
        auto const one = [&](dcon::creature_id a, uint32_t ai_chunk, dcon::creature_id b, uint32_t bj) {
            if (!consious(state, a))
                return;
            collide_with_kin(state, a, b);
            auto& ai = ai_of(state, a);
            if (!spitter(state, a) && w.creature_get_dead(b) && can_be_revived(state, b, room) && behavior(w.creature_get_behavior(a)) == behavior::revive_buddy
            && ai.revive_buddy == b) {
                auto rng = rw::random(state.seed, state.tick, rw::ids(uint32_t(a.value), uint32_t(b.value)));
                auto& mv = mv_of(state, a);
                mv.reviving_buddy = b;
                int32_t const gx = rng.range(0, 2);
                int32_t const gy = rng.range(0, rng.range(2, 4));
                mv.grab[gx][gy] = grab_chunk{b, uint8_t(bj)};
                // (no Big_Spider_Revive sound)
                return;
            }
            auto const ka = chunk(state, a, ai_chunk), kb = chunk(state, b, bj);
            if (w.body_chunk_get_pos(ka).y > w.body_chunk_get_pos(kb).y) {
                w.body_chunk_get_vel(ka).y += 2.f;
                w.body_chunk_get_vel(kb).y -= 2.f;
            }
        };
        one(c, i, other, j);
        if (w.creature_is_valid(c) && w.creature_is_valid(other))
            one(other, j, c, i);
    }

    void violence(sys::game_state& state, dcon::creature_id c, uint32_t k, vec2 momentum, float damage, float stun_bonus, creature::damage_type) {
        // (no Big_Spider_Take_Damage / Death_Rustle sounds)
        base_violence(state, c, k, momentum, damage, stun_bonus);
    }

    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t) {
        if (creature::grasped(state.world, c, 0) == t)
            lose_all_grasps(state, c);
        auto& mv = mv_of(state, c);
        for (auto& side : mv.grab)
            for (auto& g : side)
                if (g.who == t)
                    g = grab_chunk{};
        if (mv.reviving_buddy == t)
            mv.reviving_buddy = dcon::creature_id{};
    }

    void died(sys::game_state& state, dcon::creature_id c) {
        // (Creature.Die let go of everything already)
        auto& w = state.world;
        if (w.creature_get_away(c) != 0)
            return;
        if (mother(state, c))
            baby_puff(state, c, world::get_room_view(state, creature::room_of(state, c)));
    }

    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const view = world::get_room_view(state, room);
        ivec2 const t = tile_of(at);
        vec2 const a = rw::to_vec2(view.shortcut_entrance_hole_direction(t));
        vec2 const mid = room_view::middle_of_tile(t);
        for (uint32_t i = 0; i < 2; ++i) {
            auto const k = chunk(state, c, i);
            physics::hard_set_position(state, k, mid - a * (-1.5f + float(i)) * 15.f);
            w.body_chunk_set_last_pos(k, mid);
            w.body_chunk_set_vel(k, a * 2.f);
        }
        // the AI's NewRoom: what it went for is in the other room
        auto& x = pl(state, c);
        x.special_move_counter = 0;
        x.charging = 0.f;
        x.jumping = 0;
        auto& mv = mv_of(state, c);
        mv.last_followed = movement_connection{};
        mv.has_spit_pos = false;
        mv.reviving_buddy = dcon::creature_id{};
        release_all_grab_chunks(state, c);
        auto& ai = ai_of(state, c);
        ai.idle_towards = std::nullopt;
        ai.idle_pos = t;
        ai.temp_idle_pos = t;
        ai.idle_pos_counter = 0;
        ai.prev_idle_count = 0;
        ai.revive_buddy = dcon::creature_id{};
        ai.other_spider_count = 0;
        for (auto& tr : ai.track)
            tr = track_state{};
        auto& sm = spit_of(state, c);
        sm.spit_at = dcon::creature_id{};
        sm.tagged = dcon::creature_id{};
        sm.go_to_spit_pos = false;
        sm.spit_pos = t;
        sm.target_trail_count = 0;
        sm.random_crit_spit_delay = 0;
    }

    void flying_weapon(sys::game_state& state, dcon::creature_id c, vec2 wpos, vec2 wvel, vec2 thrown_pos) {
        auto& w = state.world;
        if (w.creature_get_away(c) != 0 || !consious(state, c) || spitter(state, c))
            return;
        auto& x = pl(state, c);
        if (x.jump_stamina < 0.3f || creature::grasped_object(w, c, 0))
            return;
        auto const room = world::get_room_view(state, creature::room_of(state, c));
        vec2 const c0 = pos(state, c, 0), c1 = pos(state, c, 1);
        ivec2 const t = tile_of(c0);
        if (room.get_tile(t + ivec2{0, 1}).solid() || custom::dist_less(c0, thrown_pos, 60.f))
            return;
        auto const below = room.get_tile(t + ivec2{0, -1});
        if (!below.solid() && below.t != world::terrain::floor && !room.get_tile(t).any_beam())
            return;
        if (rw::dot(rw::normalized(c1 - c0), rw::normalized(c0 - wpos)) < -0.2f)
            return;
        if (!custom::dist_less(c0, wpos, tp(state, c).visual_radius) || !room.visual_contact(c0, wpos))
            return;
        vec2 const ahead = wpos + rw::normalized(wvel) * 140.f;
        if (custom::dist_less(ahead, c0, 140.f)
        && (std::fabs(custom::distance_to_line(c0, wpos, wpos + wvel)) < 7.f || std::fabs(custom::distance_to_line(c1, wpos, wpos + wvel)) < 7.f)) {
            jump(state, c, custom::dir_vec(c0, thrown_pos + vec2{0.f, 400.f}), 1.f);
            pos(state, c, 0).y += 20.f;
            pos(state, c, 1).y += 10.f;
            x.jump_stamina = mathf::max(0.f, x.jump_stamina - 0.15f);
        }
    }

    void creature_hit_by_dart(sys::game_state& state, dcon::creature_id c, dcon::creature_id t) {
        if (!is_big_spider(state, c) || !spitter(state, c))
            return;
        auto const& ts = tracker::of(state, c);
        if (!tracker::find(ts, t))
            return;
        auto& sm = spit_of(state, c);
        auto& ai = ai_of(state, c);
        auto const me = creature::template_of(state, c);
        auto const rel = creature::static_relationship(me, creature::template_of(state, t));
        if (rel.type == creature::rel::eats
        && (!sm.tagged || !creature::live(state, sm.tagged) || creature::static_relationship(me, creature::template_of(state, sm.tagged)).intensity < rel.intensity))
            sm.tagged = t;
        track_of(ai, ts, t).tagged = until_sleep_delay + 200;
        if (sm.tagged)
            ai.stay_away = false;
        auto rng = rw::random(state.seed, state.tick, rw::ids(uint32_t(c.value), uint32_t(t.value)));
        if (rng.value() < 0.5f)
            sm.random_crit_spit_delay = 0;
    }

    std::string describe(sys::game_state const& state, dcon::creature_id c) {
        static constexpr char const* names[] = {"idle", "flee", "hunt", "escape rain", "return prey", "get unstuck", "revive buddy"};
        auto const& w = state.world;
        vec2 const p = pos(state, c, 0);
        uint32_t const b = w.creature_get_behavior(c);
        auto const& x = pl(state, c);
        std::string s = std::string(to_string(x.variant)) + " at " + std::to_string(int(p.x)) + "," + std::to_string(int(p.y)) + " behavior " + (b < 7 ? names[b] : "?");
        if (spitter(state, c))
            s += " ammo " + std::to_string(w.big_spider_data_get_spit(data(state, c)).ammo);
        if (creature::grasped_object(w, c, 0))
            s += " holding prey";
        if (x.jumping)
            s += " jumping";
        else if (x.charging > 0.f)
            s += " charging";
        if (x.sitting)
            s += " sitting";
        return s;
    }

    bool spawn_spot(room_view const& room, ivec2 tile) {
        return room.inside(tile.x, tile.y) && !room.get_tile(tile).solid() && room.get_tile(tile + ivec2{0, -1}).solid()
            && pathing::accessible(room, tile, spider_movement) && room.get_tile(tile).water == 0;
    }

    void spawn_young(sys::game_state& state, dcon::room_id room, vec2 at) {
        // a Spider (coalescipede) with bloodLust 1
        creature::spawn_desc d;
        d.kind = creature::type::spider;
        auto const sp = creature::create(state, room, at, d);
        if (sp && state.world.creature_get_type(sp) == creature::type::spider)
            state.world.creature_get_payload(sp).spider.blood_lust = 1.f;
    }
}
