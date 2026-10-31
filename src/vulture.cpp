// Vulture.cs (the body, its thrusters, its neck and wings), a simpler
// VultureAI with its DisencouragedTracker, and the hooks of the vulture
// kind: for the vulture, the king vulture and the miros vulture alike (the
// payload's variant). The tentacles are vulture_tentacle.cpp, the king's
// harpoons vulture_tusks.cpp.
//
// Not vanilla: there are no sky exits and no abstract layer here. A vulture
// roams the open air of its room and changes rooms only as migrate.hpp moves
// every creature (through the pipes); taking prey home, it flies off over
// the top of the room when the room has open sky (the prey is eaten up
// there, like in a den) and hovers out of sight for a while before it comes
// back down, else it carries the prey into its den. Fleeing the rain it
// heads for its den (the sky without one).

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "vulture.hpp"
#include "vulture_graphics.hpp"
#include "vulture_internal.hpp"
#include "daddy_internal.hpp"
#include "dens.hpp"
#include "explosions.hpp"
#include "items.hpp"
#include "migrate.hpp"
#include "particles.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "rooms.hpp"
#include "tracker.hpp"

namespace vulture {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;
    using creature::relationship;
    using daddy::detail::ray_trace_tiles_for_terrain;

    namespace detail {
        template_params const& params(variant v) noexcept {
            // CreatureTemplate: baseDamageResistance, baseStunResistance
            static constexpr template_params p[variant_count] = {{8.5f, 3.5f}, {12.5f, 6.5f}, {20.f, 5.f}};
            return p[uint32_t(v) % variant_count];
        }

        namespace {
            // the working copies, one set per nesting level: a vulture's
            // update may hurt another vulture (or itself) whose Violence
            // loads its own context meanwhile
            thread_local uint32_t depth = 0;
            constexpr uint32_t max_depth = 8;
            std::vector<wing_work>& work_buffer(uint32_t d) {
                static thread_local std::vector<wing_work> buf[max_depth];
                return buf[std::min(d, max_depth - 1)];
            }
            std::vector<ivec2>& scratch_buffer(uint32_t d) {
                static thread_local std::vector<ivec2> buf[max_depth];
                return buf[std::min(d, max_depth - 1)];
            }
            dcon::vulture_wing_id wing_data(dcon::data_container const& w, dcon::creature_part_id p) {
                return dcon::vulture_wing_id{dcon::vulture_wing_id::value_base_t(w.creature_part_get_payload(p).vulture_wing.data)};
            }
        }

        ctx::ctx(sys::game_state& s, dcon::creature_id cc, room_view const& r)
            : state(s), c(cc), room(r), rng(s.world.creature_get_random(cc)), p(s.world.creature_get_payload(cc).vulture), d(creature::vulture_data(s.world, cc)),
              b(s.world.vulture_data_get_body(creature::vulture_data(s.world, cc))), ai(s.world.vulture_data_get_ai(creature::vulture_data(s.world, cc))),
              neck_s(s.world.vulture_data_get_neck(creature::vulture_data(s.world, cc))), wings(work_buffer(depth)), scratch(scratch_buffer(depth)) {
            ++depth;
            wings.clear();
            for (uint32_t i = 0; i < chunk_count; ++i)
                ch[i] = creature::chunk(s, cc, i);
            v = p.variant;
            viewed = s.viewed(r.id);
            neck.t = &neck_s.t;
            neck.c = std::span<tentacle_chunk>{neck_s.c, neck_chunks};
            neck.tp = &neck_props;
        }

        ctx::~ctx() { --depth; }

        bool ctx::air_borne() { return !wings.empty() && all_wings(wing_mode::fly); }
        bool ctx::any_wing(wing_mode m) {
            for (auto const& ww : wings)
                if (ww.w.mode == m)
                    return true;
            return false;
        }
        bool ctx::all_wings(wing_mode m) {
            for (auto const& ww : wings)
                if (ww.w.mode != m)
                    return false;
            return true;
        }
        int32_t ctx::proximity(ivec2 t) { return pathing::ai_terrain_proximity(state, room.id, t); }
        pathing::field_id ctx::path() {
            if (!field)
                field = pathing::field_to(state, room.id, destination(), flyer);
            return *field;
        }

        void load(ctx& x) {
            auto& w = x.w();
            // the neck's tile lists
            x.neck.seg.assign(x.neck_s.segments.v, x.neck_s.segments.v + x.neck_s.segments.n);
            x.neck.gp.assign(x.neck_s.grab_path.v, x.neck_s.grab_path.v + x.neck_s.grab_path.n);
            // the wings: copied out whole (the vector settles first: the
            // spans point into its elements)
            uint32_t n = 0;
            for (auto const o : creature::parts(w, x.c))
                if (w.creature_part_get_kind(w.part_ownership_get_part(o)) == creature::part_kind::vulture_wing)
                    ++n;
            x.wings.resize(std::min<uint32_t>(n, max_wings));
            uint32_t k = 0;
            for (auto const o : creature::parts(w, x.c)) {
                auto const part = w.part_ownership_get_part(o);
                if (w.creature_part_get_kind(part) != creature::part_kind::vulture_wing || k >= x.wings.size())
                    continue;
                auto& ww = x.wings[k];
                ww.part = part;
                ww.id = wing_data(w, part);
                ww.number = w.creature_part_get_index(part);
                ww.w = w.vulture_wing_get_t(ww.id);
                auto const segs = w.vulture_wing_get_segments(ww.id);
                ww.tn.seg.assign(segs.begin(), segs.end());
                auto const gp = w.vulture_wing_get_grab_path(ww.id);
                ww.tn.gp.assign(gp.begin(), gp.end());
                ++k;
            }
            for (auto& ww : x.wings) {
                ww.tn.t = &ww.w.t;
                ww.tn.c = std::span<tentacle_chunk>{ww.w.c, ww.w.chunk_count};
                ww.tn.tp = &wing_props;
            }
            // by their number (tentacles[i])
            std::sort(x.wings.begin(), x.wings.end(), [](wing_work const& a, wing_work const& b) { return a.number < b.number; });
            for (auto& ww : x.wings) {
                ww.tn.t = &ww.w.t;
                ww.tn.c = std::span<tentacle_chunk>{ww.w.c, ww.w.chunk_count};
            }
        }

        void store(ctx& x) {
            auto& w = x.w();
            auto put = [](tile_list& l, std::vector<ivec2> const& v) {
                l.n = uint8_t(std::min<size_t>(v.size(), max_neck_tiles));
                std::copy(v.begin(), v.begin() + l.n, l.v);
            };
            put(x.neck_s.segments, x.neck.seg);
            put(x.neck_s.grab_path, x.neck.gp);
            for (auto& ww : x.wings) {
                w.vulture_wing_set_t(ww.id, ww.w);
                w.vulture_wing_get_segments(ww.id).load_range(ww.tn.seg.data(), ww.tn.seg.data() + ww.tn.seg.size());
                w.vulture_wing_get_grab_path(ww.id).load_range(ww.tn.gp.data(), ww.tn.gp.data() + ww.tn.gp.size());
            }
        }

        behavior current(ctx& x) { return behavior(x.w().creature_get_behavior(x.c)); }

        creature::tracked const* focus(ctx& x) {
            if (!x.ai.focus)
                return nullptr;
            return tracker::find(tracker::of(x.state, x.c), x.ai.focus);
        }

        creature::tracked const* most_attractive_prey(ctx& x) { return tracker::prey(tracker::of(x.state, x.c)); }

        relationship dynamic_relationship(ctx& x, dcon::creature_id t) { return tracker::dynamic_relationship(x.state, x.c, t); }

        bool charging_snap(ctx& x) { return x.p.snap_frames > 21; }

        void air_brake(ctx& x, int32_t frames) {
            x.p.landing_brake = int16_t(frames);
            x.b.landing_brake_pos = x.pos(1);
            // (Vulture_Jets_Air_Brake has no sample here)
        }

        void grab_in_beak(ctx& x, dcon::creature_id t, uint32_t k) {
            creature::grab(x.state, x.c, 0, t, k);
            // (Vulture_Grab_NPC / Vulture_Grab_Player have no sample here)
        }
    }

    using namespace detail;

    namespace {
        // CreatureTemplate.visualRadius
        constexpr float visual_radius = 7000.f;
        constexpr float float_max = std::numeric_limits<float>::max();
        constexpr ivec2 four_directions_and_zero[5] = {{0, 0}, {-1, 0}, {0, -1}, {1, 0}, {0, 1}};

        // how high over the room it hovers out of sight (shadowMode starts
        // 400 over the top)
        constexpr int32_t sky_height = 25;

        bool is_vulture_tmpl(creature::tmpl t) { return t == creature::tmpl::vulture || t == creature::tmpl::king_vulture; }

        // VultureThruster(vulture, smokeChunkA, smokeChunkB, smokeChunkLerp, angle)
        struct thruster_def {
            uint32_t a;
            uint32_t b;
            float lerp;
            float angle;
        };
        constexpr thruster_def thruster_defs[4] = {{2, 0, 0.5f, 15.f}, {3, 0, 0.5f, -15.f}, {2, 1, 0.2f, 100.f}, {3, 1, 0.2f, -100.f}};

        //
        // VultureThruster
        //
        vec2 thrust_vector(ctx& x, uint32_t i) { return custom::deg_to_vec(custom::aim(x.pos(1), x.pos(0)) + thruster_defs[i].angle); }
        bool thruster_active(ctx& x, uint32_t i) { return x.p.landing_brake > 0 || x.b.thrusters[i].thrust > 0; }
        void thruster_activate(ctx& x, uint32_t i, int32_t frames) {
            auto& t = x.b.thrusters[i];
            if (t.thrust < frames)
                t.thrust = frames;
        }
        vec2 exhaust_pos(ctx& x, uint32_t i) {
            auto const& d = thruster_defs[i];
            return rw::lerp(x.pos(d.a), x.pos(d.b), d.lerp) + thrust_vector(x, i) * 14.f;
        }
        float thruster_force(ctx& x, uint32_t i) {
            float num = mathf::min(1.f, x.p.jet_fuel * 2.f);
            num *= mathf::min(1.f, float(x.b.thrusters[i].thrust) / 5.f);
            num = mathf::pow(num, 0.4f);
            if (x.p.landing_brake <= 0 || num >= 0.5f)
                return num;
            return x.miros() ? 2.7f : 0.5f;
        }
        void set_jet_fuel(ctx& x, float v) { x.p.jet_fuel = mathf::clamp(v, 0.f, 1.f); }
        // VultureThruster.Update: the jet pushes the body (the smoke it gives
        // off is cosmetic: dust puffs where a camera looks)
        void thruster_update(ctx& x, uint32_t i) {
            auto& t = x.b.thrusters[i];
            if (t.thrust > 0)
                --t.thrust;
            bool const active = thruster_active(x, i);
            if (active) {
                vec2 const tv = thrust_vector(x, i);
                float const f = thruster_force(x, i);
                for (uint32_t k = 0; k < 4; ++k)
                    x.vel(k) -= tv * (x.king() ? 1.2f : 0.8f) * f;
                set_jet_fuel(x, x.p.jet_fuel - 1.f / (x.king() ? 60.f : 40.f));
                if (x.viewed) {
                    auto& g = x.state.runtime(x.room.id).cosmetic;
                    vec2 const ep = exhaust_pos(x, i);
                    auto const& d = thruster_defs[i];
                    if (!x.room.point_submerged(ep)) {
                        if (g.value() < 0.5f) {
                            vec2 const sv = rw::lerp(x.vel(d.a), x.vel(d.b), d.lerp) + tv * (x.king() ? 55.f : 45.f) * f;
                            particles::spawn(x.state, x.room.id, particles::kind::dust, ep, sv * 0.15f + custom::rnv(g) * g.value(), mathf::lerp(6.f, 14.f, f), 0.8f,
                                vec3{0.75f, 0.75f, 0.75f}, 0.4f);
                        }
                    } else if (g.value() < 0.3f) {
                        particles::bubbles(x.state, x.room.id, ep, 1, 5.f);
                    }
                }
                // (Vulture_Jet_Start, Vulture_Jet_LOOP have no sample here)
            }
            t.last_active = active ? 1 : 0;
        }
        float thruster_utility(ctx& x, uint32_t i) {
            vec2 a{0.f, 0.f};
            for (uint32_t k = 0; k < chunk_count; ++k)
                a += x.vel(k);
            a /= float(chunk_count);
            return mathf::max(0.f, -rw::dot(x.p.move_direction, rw::normalized(a))) * mathf::max(0.f, rw::dot(x.p.move_direction, -thrust_vector(x, i)));
        }

        //
        // VultureAI
        //
        tracker::params const& tracker_params(variant v) {
            // Tracker(this, 10, 5, -1, 0.5f, 5, 5, 10), PreyTracker(this, 5,
            // 1.1 / 1.5 / 1.8, 60, 800 / 600, ...)
            static std::array<tracker::params, variant_count> const p = [] {
                tracker::params q[variant_count];
                for (uint32_t i = 0; i < variant_count; ++i) {
                    q[i].see_around_corners = 10;
                    q[i].frames_to_remember = -1;
                    q[i].ghost_dismissal_range = 10;
                    q[i].prey_max = 5;
                    q[i].sure_to_get = 60.f;
                    q[i].sure_to_lose = 800.f;
                }
                q[0].persistance_bias = 1.1f;
                q[1].persistance_bias = 1.5f;
                q[2].persistance_bias = 1.8f;
                q[2].sure_to_lose = 600.f;
                std::array<tracker::params, variant_count> r;
                for (uint32_t i = 0; i < variant_count; ++i)
                    r[i] = q[i];
                return r;
            }();
            return p[uint32_t(v) % variant_count];
        }

        // VultureAI.VisualScore > 0 and the room's visual contact, from the head
        bool visual_contact(ctx& x, vec2 look) {
            vec2 const eye = x.pos(head);
            if (!custom::dist_less(eye, look, visual_radius))
                return false;
            float score = mathf::inverse_lerp(visual_radius, 0.f, rw::distance(eye, look));
            if (!custom::dist_less(x.pos(0), look, 40.f)) {
                vec2 const tip = x.neck.tip().pos;
                score -= mathf::pow(mathf::inverse_lerp(1.f, -0.3f, rw::dot(rw::normalized(tip - eye), rw::normalized(tip - look))), x.king() ? 0.5f : 0.15f);
            }
            return score > 0.f && x.room.visual_contact(eye, look);
        }

        // VultureState.mask of another vulture
        bool mask_of(sys::game_state const& state, dcon::creature_id t) {
            return creature::is(state.world, t, creature::type::vulture) && state.world.creature_get_payload(t).vulture.mask != 0;
        }

        // VultureAI.UpdateDynamicRelationship
        relationship relate(ctx& x, creature::tracked const& rep) {
            auto const t = rep.who;
            auto const tt = creature::template_of(x.state, t);
            bool const mask = x.p.mask != 0;
            bool const t_alive = !x.w().creature_get_dead(t);
            if (!x.miros() && is_vulture_tmpl(tt) && t_alive && mask != mask_of(x.state, t))
                return relationship{creature::rel::attacks, mask ? 0.8f : 0.1f};
            relationship const st = creature::static_relationship(x.state, x.c, t);
            if (mask && !x.miros()) {
                relationship r = st;
                if (tt == creature::tmpl::slugcat) {
                    // (story difficulty 0: LerpMap(0, -1, 1, 0.2, 0.5))
                    if (x.state.story_slugcat == player::slugcat_name::yellow)
                        r = relationship{creature::rel::eats, 0.15f};
                    else
                        r = relationship{creature::rel::eats, 0.35f + (x.king() ? 0.15f : 0.f)};
                }
                if (x.king() && r.type == creature::rel::eats) {
                    if (any_impaled(x)) {
                        r.intensity = impaled(x, t) ? 1.f : 0.5f;
                    } else {
                        auto const& k = x.w().vulture_data_get_tusks(x.d);
                        if (k.target && tusks_ready_to_shoot(x) && k.eyes_home_in > 0.f) {
                            auto const* tr = tracker::find(tracker::of(x.state, x.c), k.target);
                            if (tr && tr->ticks_since_seen < 30)
                                r.intensity = mathf::pow(r.intensity, k.target == t ? 0.5f : 1.5f);
                        }
                    }
                }
                return r;
            }
            // SocialMemory: the slugcat that knocked its mask off (here: any
            // slugcat, once the mask is gone)
            if (!mask && !x.miros() && tt == creature::tmpl::slugcat)
                return relationship{creature::rel::attacks, 1.f};
            if (x.miros() && tt == creature::tmpl::slugcat)
                return relationship{creature::rel::eats, 1.f};
            return relationship{st.type, st.intensity * (x.miros() ? 1.f : 0.1f)};
        }

        // IUseARelationshipTracker.ModuleToTrackRelationship
        creature::track_module module_of(relationship r) noexcept {
            if (r.type == creature::rel::eats || r.type == creature::rel::attacks)
                return creature::track_module::prey;
            return creature::track_module::none;
        }

        bool hook_sees(void* u, dcon::body_chunk_id k) {
            auto& x = *static_cast<ctx*>(u);
            return visual_contact(x, x.state.world.body_chunk_get_pos(k));
        }
        bool hook_sees_point(void* u, vec2 p) { return visual_contact(*static_cast<ctx*>(u), p); }
        relationship hook_relate(void* u, creature::tracked const& rep) { return relate(*static_cast<ctx*>(u), rep); }
        bool hook_reach(void* u, ivec2 t) {
            auto& x = *static_cast<ctx*>(u);
            if (!x.accessible(t) || x.destination().y >= x.room.height)
                return x.accessible(t);
            return pathing::distance(x.state, x.path(), t) != pathing::unreachable;
        }
        tracker::hooks make_hooks(ctx& x) {
            tracker::hooks h;
            h.user = &x;
            h.sees = hook_sees;
            h.sees_point = hook_sees_point;
            h.relate = hook_relate;
            h.module = module_of;
            h.reach = hook_reach;
            return h;
        }

        // VultureAI.OnlyHurtDontGrab
        bool only_hurt_dont_grab(ctx& x, dcon::creature_id t) {
            auto const* r = tracker::find(tracker::of(x.state, x.c), t);
            return r && r->rel.type == creature::rel::attacks;
        }

        // VultureAI.DoIWantToBiteCreature
        bool wants_to_bite(ctx& x, dcon::creature_id t) { return x.miros() && !creature::small_creature(creature::template_of(x.state, t)); }

        // the open column over the room nearest `from_x` (the top row tile and
        // the three below it open), -1 none: the way out to the sky
        int32_t sky_column(room_view const& room, int32_t from_x) {
            int32_t const y = room.height - 1;
            auto const open = [&](int32_t xx) {
                for (int32_t k = 0; k < 4; ++k)
                    if (room.get_tile(xx, y - k).solid())
                        return false;
                return true;
            };
            for (int32_t d = 0; d < room.width; ++d) {
                int32_t const a = from_x - d, b = from_x + d;
                if (a >= 0 && a < room.width && open(a))
                    return a;
                if (b >= 0 && b < room.width && open(b))
                    return b;
            }
            return -1;
        }

        // heading for the sky: the column it flies up through (false: no sky
        // it can get to)
        bool go_to_sky(ctx& x) {
            if (x.ai.sky_x < 0 || x.ai.sky_x >= x.room.width) {
                int32_t const col = sky_column(x.room, custom::int_clamp(x.at().x, 0, x.room.width - 1));
                if (col < 0)
                    return false;
                x.ai.sky_x = col;
            }
            x.set_destination(ivec2{x.ai.sky_x, x.room.height + sky_height});
            return true;
        }

        // the roaming spot (not vanilla: there its abstract AI moves it from
        // room to room through the sky exits): open air away from the terrain
        void roam(ctx& x, ivec2 from) {
            auto& ai = x.ai;
            --ai.idle_counter;
            if (custom::manhattan_distance(from, ai.idle_pos) < 3 && ai.idle_counter > 160)
                ai.idle_counter = x.rng.range(20, 160);
            if (ai.idle_counter <= 0 || !x.accessible(ai.idle_pos)) {
                // now and then off to the sky a while
                if (ai.idle_counter <= 0 && x.rng.value() < 0.15f && sky_column(x.room, custom::int_clamp(from.x, 0, x.room.width - 1)) >= 0) {
                    ai.sky_x = -1;
                    ai.sky_counter = x.rng.range(400, 1600);
                    ai.idle_counter = x.rng.range(200, 800);
                    return;
                }
                for (int32_t k = 0; k < 3; ++k) {
                    ivec2 const t{x.rng.range(0, x.room.width), x.rng.range(0, x.room.height)};
                    if (!x.accessible(t) || x.proximity(t) < 4)
                        continue;
                    if (x.accessible(from) && pathing::distance(x.state, pathing::field_to(x.state, x.room.id, t, flyer), from) == pathing::unreachable)
                        continue;
                    ai.idle_pos = t;
                    ai.idle_counter = x.rng.range(200, 800);
                    break;
                }
            }
            x.set_destination(x.accessible(ai.idle_pos) ? ai.idle_pos : from);
        }

        // VultureAI.KingTuskShootPosScore
        float king_tusk_shoot_pos_score(ctx& x, ivec2 test, ivec2 prey_tile) {
            if (!x.accessible(test) || pathing::distance(x.state, pathing::field_to(x.state, x.room.id, test, flyer), x.accessible(x.at()) ? x.at() : test) == pathing::unreachable)
                return float_max;
            float const num = rw::distance(room_view::middle_of_tile(test), room_view::middle_of_tile(prey_tile));
            float num2 = std::fabs(shoot_range * 0.9f - num);
            num2 += rw::distance(room_view::middle_of_tile(test), room_view::middle_of_tile(x.ai.king_tusk_shoot_pos)) / 40.f;
            if (test.y >= x.room.height - 1)
                num2 += 1000.f;
            num2 -= float(std::min(x.proximity(test), 10)) * 20.f;
            num2 += custom::lerp_map(num, shoot_range - 100.f, shoot_range, 0.f, 1000.f);
            num2 += custom::lerp_map(num, min_shoot_range, min_shoot_range / 2.f, 0.f, 1000.f);
            if (!x.room.visual_contact(room_view::middle_of_tile(test), room_view::middle_of_tile(prey_tile)))
                num2 += 10000.f;
            return num2;
        }

        // Vulture.Snap / SnapTowards
        void snap(ctx& x, dcon::creature_id t, uint32_t k) {
            x.p.tusk_charge = 0.f;
            x.b.snap_at = t;
            x.b.snap_at_chunk = uint8_t(k);
            x.p.snap_frames = 49;
            // (Vulture_Peck has no sample here)
        }

        // the creature it home in on (an ElaborateCreatureRepresentation's
        // realized chunks), the point when it snaps towards nothing
        vec2 snap_pos(ctx& x) {
            auto const t = x.b.snap_at;
            if (t && creature::here(x.state, t, x.room.id) && x.b.snap_at_chunk < creature::chunk_count_of(x.state, t))
                return x.w().body_chunk_get_pos(creature::chunk_of(x.state, t, x.b.snap_at_chunk));
            return x.b.snap_at_pos;
        }

        // VultureAI.Update
        void ai_update(ctx& x) {
            auto& w = x.w();
            auto& ai = x.ai;
            ivec2 const from = x.at();
            tracker::hooks const h = make_hooks(x);
            tracker::params const& tp = tracker_params(x.v);
            tracker::update(x.state, x.c, x.room, from, tp, h);
            auto const& ts = tracker::of(x.state, x.c);
            ++ai.time_in_room;
            if (x.state.story_slugcat == player::slugcat_name::yellow)
                ++ai.time_in_room;
            ai.disencouraged = mathf::max(0.f, ai.disencouraged - 1.f / mathf::lerp(600.f, 4800.f, ai.disencouraged));
            // DisencouragedTracker.Update
            if (ai.disencouraged > 1.f)
                ai.disencouraged = mathf::lerp(ai.disencouraged, 1.f, x.miros() ? 0.075f : 0.05f);
            ai.prey_in_tusk_charge_range = 0;
            float const t = float(ai.time_in_room);
            float const prey_weight = 0.05f + 0.95f * (x.miros() ? mathf::inverse_lerp(4000.f, 7600.f, t) : mathf::inverse_lerp(9600.f, 6000.f, t));
            float const dis_weight = 0.95f + (x.miros() ? mathf::inverse_lerp(2000.f, 13600.f, t) : 0.f);
            // UtilityComparer: the prey, the rain (here its own behavior), the
            // disencouraged tracker (the stuck tracker isn't ported)
            behavior b = behavior::idle;
            float best = 0.01f;
            if (tracker::prey(ts)) {
                float const u = tracker::prey_utility(x.state, x.c, x.room, from, tp) * prey_weight;
                if (u > best) {
                    best = u;
                    b = behavior::hunt;
                }
            }
            if (float const u = dens::rain_utility(x.state, x.c, x.room.id); u > best) {
                best = u;
                b = behavior::escape_rain;
            }
            if (float const u = mathf::pow(mathf::clamp01(ai.disencouraged), 3.f) * dis_weight; u > best) {
                best = u;
                b = behavior::disencouraged;
            }
            w.creature_set_current_utility(x.c, best);
            // what it carries in its beak and eats goes home
            if (auto const held = x.grasped(); held && creature::static_relationship(x.state, x.c, held).type == creature::rel::eats)
                b = behavior::return_prey;
            // up over the room a while (not vanilla)
            if (b == behavior::idle && ai.sky_counter > 0)
                b = behavior::sky;
            if (b != behavior::sky && b != behavior::return_prey && b != behavior::escape_rain && b != behavior::disencouraged)
                ai.sky_x = -1;
            w.creature_set_behavior(x.c, uint8_t(b));
            switch (b) {
            case behavior::idle:
                ai.focus = dcon::creature_id{};
                migrate::idle_roll(x.state, x.c, x.room);
                if (auto const tt = migrate::travel_tile(x.state, x.c, x.room)) {
                    x.set_destination(*tt);
                    break;
                }
                roam(x, from);
                break;
            case behavior::sky:
                --ai.sky_counter;
                if (!go_to_sky(x)) {
                    ai.sky_counter = 0;
                    roam(x, from);
                }
                break;
            case behavior::return_prey:
            case behavior::disencouraged:
                ai.focus = dcon::creature_id{};
                // off over the top of the room, else home to the den
                if (!go_to_sky(x)) {
                    if (auto const tt = dens::home_tile(x.state, x.c, x.room))
                        x.set_destination(*tt);
                    else
                        roam(x, from);
                }
                break;
            case behavior::escape_rain:
                ai.focus = dcon::creature_id{};
                // the den, else the sky
                if (auto const tt = dens::home_tile(x.state, x.c, x.room))
                    x.set_destination(*tt);
                else if (!go_to_sky(x))
                    roam(x, from);
                break;
            case behavior::hunt: {
                auto const* prey = tracker::prey(ts);
                ai.focus = prey->who;
                if (prey->rel.type == creature::rel::attacks)
                    ai.time_in_room = 0;
                ivec2 goal = tracker::best_guess(*prey);
                // another vulture: its head
                auto const pt = creature::template_of(x.state, prey->who);
                bool const flag = x.miros() ? pt == creature::tmpl::miros_vulture : is_vulture_tmpl(pt);
                if (flag && prey->visual && creature::here(x.state, prey->who, x.room.id) && creature::chunk_count_of(x.state, prey->who) > head)
                    goal = room_view::get_tile_position(w.body_chunk_get_pos(creature::chunk_of(x.state, prey->who, head)));
                if (x.king() && tusks_ready_to_shoot(x) && prey->ticks_since_seen < 80 && tusks_want_to_shoot(x, false, true)) {
                    ivec2 const pg = tracker::best_guess(*prey);
                    float const r1 = x.rng.value(), r2 = x.rng.value(), r3 = x.rng.value();
                    vec2 const off = custom::deg_to_vec(180.f * r1 * r2 * (r3 < 0.5f ? -1.f : 1.f)) * mathf::lerp(min_shoot_range, shoot_range, x.rng.value());
                    ivec2 const cand = room_view::get_tile_position(room_view::middle_of_tile(pg) + off);
                    if (king_tusk_shoot_pos_score(x, cand, pg) < king_tusk_shoot_pos_score(x, ai.king_tusk_shoot_pos, pg))
                        ai.king_tusk_shoot_pos = cand;
                    x.set_destination(ai.king_tusk_shoot_pos);
                } else {
                    x.set_destination(goal);
                }
                if (prey->visual && creature::here(x.state, prey->who, x.room.id)) {
                    uint32_t const n = creature::chunk_count_of(x.state, prey->who);
                    if (n > 0) {
                        uint32_t const k = uint32_t(x.rng.range(0, int32_t(n)));
                        vec2 const kp = w.body_chunk_get_pos(creature::chunk_of(x.state, prey->who, k));
                        ai.prey_in_tusk_charge_range = custom::dist_less(x.pos(0), kp, 230.f) ? 1 : 0;
                        bool const laser = !x.dead() && x.p.laser_counter > 0 && !x.grasped();
                        if ((!x.air_borne() || x.rng.value() < 0.016666668f) && x.p.tusk_charge == 1.f && x.p.snap_frames == 0 && !laser && custom::dist_less(x.pos(0), kp, 130.f)
                            && x.room.visual_contact(x.pos(head), kp))
                            snap(x, prey->who, k);
                    }
                }
                break;
            }
            case behavior::get_unstuck:
                break;
            }
        }

        //
        // Vulture
        //
        bool laser_active(ctx& x) { return !x.dead() && x.p.laser_counter > 0 && !x.grasped(); }
        bool shadow_mode(ctx& x) { return x.pos(0).y > x.room.pixel_height() + 400.f; }

        // Vulture.Stun (+ Creature.Stun)
        void stun(ctx& x, int32_t st) {
            x.p.snap_frames = 0;
            if (x.miros())
                creature::release_all(x.state, x.c, x.room.id);
            int32_t& s = x.w().creature_get_stun(x.c);
            s = std::max(s, st);
        }

        // Vulture.FireLaser (without its graphics module: it fires where no
        // camera looks too)
        void fire_laser(ctx& x) {
            if (x.p.laser_counter <= 0 && !x.dead() && x.mostly_consious() && !shadow_mode(x))
                x.p.laser_counter = 200;
        }

        // Vulture.LaserExplosion: where the beam meets the terrain
        void laser_explosion(ctx& x) {
            vec2 const p = x.pos(head);
            vec2 const a = custom::dir_vec(x.neck.tip().pos, p);
            bool hit = false;
            vec2 const corner = beam_end(x.room, p, a, &hit);
            if (!hit)
                return;
            // Explosion(room, this, corner, 7, 250, 6.2, 2, 280, ...), its
            // lights, shock wave, sparks, smoke and fragments as the port's
            // explosion
            explosions::explode(x.state, x.room, corner, explosions::desc{250.f, 6.2f, 2.f, 280.f, 150.f}, creature::body(x.state, x.c));
        }

        // Vulture.CheckFlip: the wing bases swapped when the body turned over
        void check_flip(ctx& x) {
            auto& w = x.w();
            if (custom::distance_to_line(x.pos(2), x.pos(0), x.pos(1)) >= 0.f)
                return;
            auto const a = x.ch[2], b = x.ch[3];
            std::swap(w.body_chunk_get_pos(a), w.body_chunk_get_pos(b));
            std::swap(w.body_chunk_get_vel(a), w.body_chunk_get_vel(b));
            std::swap(w.body_chunk_get_last_pos(a), w.body_chunk_get_last_pos(b));
        }

        // Vulture.TakeOff
        void take_off(ctx& x) {
            x.b.time_since_last_takeoff = 0;
            for (auto& ww : x.wings)
                switch_mode(x, ww, wing_mode::fly);
            // (VultureGraphics.MakeColorWave: asked for, the module makes it)
            x.b.color_wave_delay = x.rng.range(30, 60);
            ++x.b.color_waves;
            air_brake(x, 5);
            thruster_activate(x, 0, 30);
            thruster_activate(x, 1, 30);
        }

        // Vulture.TentacleMaxReleaseInd
        wing_work& max_release(ctx& x) {
            float num = -1.f;
            uint32_t result = 0;
            for (uint32_t i = 0; i < x.wings.size(); ++i) {
                float const s = release_score(x, x.wings[i]);
                if (s > num || num == -1.f) {
                    num = s;
                    result = i;
                }
            }
            return x.wings[result];
        }

        // Vulture.JawSlamShut: the miros vulture's jaws snap shut on what is
        // in front of them
        void jaw_slam_shut(ctx& x) {
            auto& w = x.w();
            vec2 const a = custom::dir_vec(x.neck.tip().pos, x.pos(head));
            x.neck.tip().vel -= a * 10.f;
            x.neck.tip().pos += a * 20.f;
            x.pos(head) += a * 20.f;
            // (the Miros_Beak_Snap sounds have no sample here)
            rooms::for_each_creature(x.state, x.room.id, [&](dcon::creature_id t) {
                if (x.grasped() || t == x.c || !creature::here(x.state, t, x.room.id) || !wants_to_bite(x, t) || w.creature_get_entering_shortcut(t).x >= 0)
                    return;
                uint32_t const m = creature::chunk_count_of(x.state, t);
                for (uint32_t k = 0; k < m; ++k) {
                    auto const kc = creature::chunk_of(x.state, t, k);
                    vec2 const kp = w.body_chunk_get_pos(kc);
                    if (custom::dist_less(x.pos(head) + a * 20.f, kp, 20.f + w.body_chunk_get_rad(kc)) && x.room.visual_contact(x.pos(head), kp)) {
                        vec2 const mom = custom::dir_vec(x.pos(head), kp) * 4.f;
                        if (!only_hurt_dont_grab(x, t)) {
                            creature::grab(x.state, x.c, 0, t, k);
                            x.p.jaw_open = 0.15f;
                            x.b.jaw_vel = 0.f;
                            creature::violence_on(x.state, t, k, mom, 1.2f, 30.f, creature::damage_type::bite, creature::tmpl::miros_vulture);
                        } else {
                            creature::violence_on(x.state, t, k, mom, 1.2f, 20.f, creature::damage_type::bite, creature::tmpl::miros_vulture);
                        }
                        break;
                    }
                }
                // (a daddy's tentacles bitten aren't stunned)
            });
        }

        // the look creature (CreatureLooker: here its focus) or the destination
        vec2 look_point(ctx& x) {
            if (auto const* f = focus(x)) {
                if (f->visual && creature::here(x.state, f->who, x.room.id))
                    return creature::main_pos(x.state, f->who);
                return room_view::middle_of_tile(tracker::best_guess(*f));
            }
            return room_view::middle_of_tile(x.destination());
        }

        // Vulture.UpdateNeck
        void update_neck(ctx& x) {
            auto& n = x.neck;
            auto& t = *n.t;
            tentacle_update(x, n);
            // (StuckTracker.CloseToGoalButNotSeeingItTracker isn't ported)
            x.w().body_chunk_set_collide_with_terrain(x.ch[head], 1);
            bool const air = x.air_borne();
            for (uint32_t j = 0; j < neck_chunks; ++j) {
                auto& c = n.c[j];
                c.vel *= 0.95f;
                c.vel.y -= t.limp ? 0.7f : 0.1f;
                c.vel += custom::dir_vec(x.pos(1), x.pos(0)) * (j == 0 ? 1.2f : 0.8f);
                c.vel -= x.vel(0);
                c.vel *= air ? 0.2f : 0.75f;
                c.vel += x.vel(0);
            }
            bool const awake = x.awake();
            t.limp = awake ? 0 : 1;
            float const num2 = t.backtrack_from == -1 ? 0.5f : 0.f;
            auto& tip = n.tip();
            auto& mid = n.c[neck_chunks - 2];
            auto const held = x.grasped();
            if (!held) {
                vec2 const a = custom::dir_vec(x.pos(head), tip.pos);
                float const num3 = rw::distance(x.pos(head), tip.pos);
                x.pos(head) -= (6.f - num3) * a * (1.f - num2);
                x.vel(head) -= (6.f - num3) * a * (1.f - num2);
                tip.pos += (6.f - num3) * a * num2;
                tip.vel += (6.f - num3) * a * num2;
                x.vel(head) += custom::dir_vec(mid.pos, x.pos(head)) * (air ? 2.f : 6.f) * (1.f - num2);
                x.vel(head) += custom::dir_vec(tip.pos, x.pos(head)) * (air ? 2.f : 6.f) * (1.f - num2);
                tip.vel -= custom::dir_vec(mid.pos, x.pos(head)) * (air ? 1.f : 3.f) * num2;
                mid.vel -= custom::dir_vec(mid.pos, x.pos(head)) * (air ? 1.f : 3.f) * num2;
            }
            if (!awake)
                return;
            vec2 const sp = snap_pos(x);
            if (charging_snap(x)) {
                x.vel(head) += (x.pos(0) + custom::dir_vec(x.pos(0), sp) * 50.f - x.pos(head)) / 6.f;
                tip.vel -= custom::dir_vec(x.pos(head), sp) * 10.f * num2;
                return;
            }
            if (x.p.snap_frames > 0) {
                x.vel(head) += custom::dir_vec(x.pos(head), sp) * 15.f;
                tip.vel -= custom::dir_vec(x.pos(head), sp) * num2;
                return;
            }
            vec2 const v = look_point(x);
            if (custom::dist_less(v, x.pos(0), 220.f) && !x.room.visual_contact(v, x.pos(head)))
                move_grab_dest(x, n, v);
            else if (t.backtrack_from == -1)
                t.float_grab_dest = std::nullopt;
            vec2 const a2 = custom::dir_vec(x.pos(head), v);
            if (!held) {
                tip.vel += a2 * num2;
                mid.vel -= a2 * 0.5f * num2;
                x.vel(head) += a2 * 4.f * (1.f - num2);
            } else {
                tip.vel += a2 * 2.f * num2;
                mid.vel -= a2 * 2.f * num2;
                if (creature::here(x.state, held, x.room.id) && creature::grasp_chunk(x.w(), x.c, 0) < creature::chunk_count_of(x.state, held)) {
                    auto const gc = creature::chunk_of(x.state, held, creature::grasp_chunk(x.w(), x.c, 0));
                    x.w().body_chunk_get_vel(gc) += a2 / x.w().body_chunk_get_mass(gc);
                }
            }
            if (custom::dist_less(x.pos(head), v, 80.f))
                for (auto& c : n.c)
                    c.vel -= a2 * mathf::inverse_lerp(80.f, 20.f, rw::distance(x.pos(head), v)) * 8.f * num2;
        }

        // Vulture.Carry: what the beak holds is dragged along
        void carry(ctx& x) {
            auto& w = x.w();
            if (!x.awake()) {
                creature::release_all(x.state, x.c, x.room.id);
                return;
            }
            auto const t = x.grasped();
            uint32_t const k = creature::grasp_chunk(w, x.c, 0);
            if (!creature::here(x.state, t, x.room.id) || k >= creature::chunk_count_of(x.state, t)) {
                creature::release_all(x.state, x.c, x.room.id);
                return;
            }
            // (a Gourmand is dropped ten times as often: not told apart here)
            if (x.rng.value() < 0.008333334f && creature::static_relationship(x.state, x.c, t).type != creature::rel::eats) {
                creature::release_all(x.state, x.c, x.room.id);
                return;
            }
            auto const gc = creature::chunk_of(x.state, t, k);
            vec2& gp = w.body_chunk_get_pos(gc);
            vec2& gv = w.body_chunk_get_vel(gc);
            float const gm = w.body_chunk_get_mass(gc);
            float num2 = gm / (gm + x.mass(head));
            float num3 = gm / (gm + x.mass(0));
            if (x.neck.t->backtrack_from != -1) {
                num2 = 0.f;
                num3 = 0.f;
            }
            auto& tip = x.neck.tip();
            if (!custom::dist_less(gp, tip.pos, 20.f)) {
                vec2 const a = custom::dir_vec(gp, tip.pos);
                float const num4 = rw::distance(gp, tip.pos);
                gp -= (20.f - num4) * a * (1.f - num2);
                gv -= (20.f - num4) * a * (1.f - num2);
                tip.pos += (20.f - num4) * a * num2;
                tip.vel += (20.f - num4) * a * num2;
            }
            x.pos(head) = rw::lerp(tip.pos, gp, 0.1f);
            x.vel(head) = tip.vel;
            constexpr float num5 = 70.f;
            if (!custom::dist_less(x.pos(0), gp, num5)) {
                vec2 const a2 = custom::dir_vec(gp, x.pos(0));
                float const num6 = rw::distance(gp, x.pos(0));
                gp -= (num5 - num6) * a2 * (1.f - num3);
                gv -= (num5 - num6) * a2 * (1.f - num3);
                x.pos(0) += (num5 - num6) * a2 * num3;
                x.vel(0) += (num5 - num6) * a2 * num3;
            }
            if (w.creature_get_entering_shortcut(t).x >= 0)
                creature::release_all(x.state, x.c, x.room.id);
        }

        // Vulture.StuckBehavior: shaking itself loose
        void stuck_behavior(ctx& x) {
            bool flag = false;
            for (uint32_t i = 0; i < 4 && !flag; ++i)
                flag = x.contact(i).x != 0 || x.contact(i).y != 0 || (x.room.get_tile(x.pos(i)).flags & world::tile_worm_grass) != 0;
            if (flag) {
                ivec2 const mt = room_view::get_tile_position(x.pos(0));
                for (int32_t j = 2; j < 5; ++j)
                    for (float num = 0.f; num < 360.f; num += 36.f)
                        if (ray_trace_tiles_for_terrain(x.room, mt, room_view::get_tile_position(x.pos(0) + custom::deg_to_vec(num) * 20.f * float(j)))) {
                            for (uint32_t k = 0; k < 4; ++k)
                                x.vel(k) += custom::deg_to_vec(num);
                            break;
                        }
            }
            vec2 const b = custom::deg_to_vec(x.rng.value() * 360.f) * 2.f;
            for (uint32_t l = 0; l < 4; ++l)
                x.vel(l) += b;
            bool none = true;
            for (uint32_t m = 0; m < 4; ++m)
                none = none && !thruster_active(x, m);
            if (x.p.jet_fuel > 0.2f && none)
                thruster_activate(x, uint32_t(x.rng.range(0, 4)), 5 + x.rng.range(0, 10));
        }

        // a step of the way (MovementConnection: from the start tile to the next)
        struct connection {
            ivec2 start;
            ivec2 dest;
        };

        int32_t sign0(int32_t v) { return (v > 0) - (v < 0); }

        // VulturePather.FollowPath over the flying field to the destination;
        // out of the room it heads back in, or on to a destination outside
        // (the sky over the room: straight up an open column)
        std::optional<connection> follow_path(ctx& x, ivec2 from) {
            ivec2 const dest = x.destination();
            int32_t const w = x.room.width, h = x.room.height;
            if (!x.room.inside(from.x, from.y)) {
                if (from.y >= h) {
                    if (dest.y >= h) {
                        ivec2 const d{sign0(dest.x - from.x), sign0(dest.y - from.y)};
                        if (d == ivec2{0, 0})
                            return std::nullopt;
                        return connection{from, from + d};
                    }
                    return connection{from, ivec2{custom::int_clamp(from.x + sign0(dest.x - from.x), 0, w - 1), from.y - 1}};
                }
                ivec2 const to{custom::int_clamp(from.x, 0, w - 1), custom::int_clamp(from.y, 0, h - 1)};
                return connection{from, from + custom::clamp_at_one(to - from)};
            }
            if (dest.y >= h) {
                // the column over it open to the top: straight up
                if (!x.room.get_tile(from).solid() && ray_trace_tiles_for_terrain(x.room, from, ivec2{from.x, h - 1}))
                    return connection{from, from + ivec2{0, 1}};
                if (x.ai.sky_x < 0 || !x.accessible(from))
                    return std::nullopt;
                auto const f = pathing::field_to(x.state, x.room.id, ivec2{x.ai.sky_x, h - 1}, flyer);
                if (auto const s = pathing::next_step(x.state, f, from))
                    return connection{from, *s};
                return std::nullopt;
            }
            if (!x.accessible(from) || !x.room.inside(dest.x, dest.y))
                return std::nullopt;
            if (auto const s = pathing::next_step(x.state, x.path(), from))
                return connection{from, *s};
            return std::nullopt;
        }

        // AItile.fallRiskTile within ten tiles below (a floor to land on)
        bool fall_risk_close(ctx& x, ivec2 t) {
            if (!x.room.inside(t.x, t.y))
                return false;
            return pathing::floor_altitude(x.room, t) < 10;
        }

        // Vulture.Act
        void act(ctx& x) {
            auto& p = x.p;
            auto& b = x.b;
            auto& rng = x.rng;
            ai_update(x);
            if (x.miros()) {
                p.last_jaw_open = p.jaw_open;
                if (x.grasped()) {
                    p.jaw_open = 0.15f;
                } else if (b.jaw_slam_pause > 0) {
                    --b.jaw_slam_pause;
                } else {
                    if (laser_active(x)) {
                        b.jaw_keep_open_pause = 10;
                        b.keep_jaw_open_pos = 1.f;
                    }
                    if (b.jaw_vel == 0.f)
                        b.jaw_vel = 0.15f;
                    p.jaw_open += b.jaw_vel;
                    if (b.jaw_keep_open_pause > 0) {
                        --b.jaw_keep_open_pause;
                        p.jaw_open = mathf::clamp(mathf::lerp(p.jaw_open, b.keep_jaw_open_pos, rng.value() * 0.5f), 0.f, 1.f);
                    } else if (rng.value() < 1.f / 40.f) {
                        b.jaw_keep_open_pause = rng.range(10, rng.range(10, 60));
                        b.keep_jaw_open_pos = rng.value() >= 0.5f ? 1.f : 0.f;
                        b.jaw_vel = mathf::lerp(-0.4f, 0.4f, rng.value());
                        p.jaw_open = mathf::clamp(p.jaw_open, 0.f, 1.f);
                    } else if (p.jaw_open <= 0.f) {
                        p.jaw_open = 0.f;
                        if (b.jaw_vel < -0.4f)
                            jaw_slam_shut(x);
                        b.jaw_vel = 0.15f;
                        b.jaw_slam_pause = 5;
                    } else if (p.jaw_open >= 1.f) {
                        p.jaw_open = 1.f;
                        b.jaw_vel = -0.5f;
                    }
                }
            }
            if (!x.air_borne()) {
                constexpr float num = 100.f;
                vec2 const m = x.pos(0);
                if (m.x < -num || m.y < -num || m.x > x.room.pixel_width() + num || m.y > x.room.pixel_height() + num)
                    for (auto& ww : x.wings)
                        switch_mode(x, ww, wing_mode::fly);
            }
            p.wing_flap += p.wing_flap < 0.5f ? 0.033333335f : 0.02f;
            if (p.wing_flap > 1.f)
                p.wing_flap -= 1.f;
            float num2 = 0.f;
            for (auto const& ww : x.wings)
                num2 += support(x, ww) * (x.miros() ? 0.75f : 0.5f);
            num2 = mathf::pow(num2, 0.5f);
            num2 = mathf::max(num2, 0.1f);
            p.hover_still = 0;
            ivec2 iv = room_view::get_tile_position(x.pos(0));
            for (auto const d : four_directions_and_zero)
                if (x.accessible(iv + d))
                    iv += d;
            auto const conn = follow_path(x, iv);
            ivec2 const at = x.at();
            if (!conn || custom::manhattan_distance(at, x.destination()) < 2)
                p.hover_still = 1;
            x.neck.t->retract_fac = mathf::clamp(x.neck.t->retract_fac + 0.033333335f, 0.f, 0.6f);
            x.vel(head) *= 0.9f;
            bool const air = x.air_borne();
            for (uint32_t k = 0; k < 4; ++k) {
                if (air) {
                    x.vel(k) *= 0.98f;
                    num2 = 0.f;
                    for (auto const& ww : x.wings)
                        num2 += x.wing_stun(ww) >= 5.f ? 0.f : 1.f / float(x.wing_count());
                } else {
                    x.vel(k) *= mathf::lerp(0.98f, 0.9f, num2);
                    if (num2 > 0.1f)
                        x.vel(k).y += mathf::lerp(1.2f, 0.5f, num2);
                }
            }
            float const speed = rw::magnitude(x.vel(0));
            x.vel(1).y += 1.9f * num2 * mathf::inverse_lerp(1.f, 7.f, speed);
            x.vel(0).y -= 1.9f * num2 * mathf::inverse_lerp(1.f, 7.f, speed);
            if (!p.hover_still && (!conn || (conn->dest == b.last_connection && x.room.inside(at.x, at.y)))) {
                ++b.stuck;
                if (b.stuck > 60) {
                    p.stuck_shake = int16_t(std::min(b.stuck_shake_duration, 30000));
                    b.stuck_shake_duration += 30;
                }
            } else {
                b.stuck = 0;
                if (b.stuck_shake_duration > 30)
                    --b.stuck_shake_duration;
            }
            for (auto const d : four_directions_and_zero) {
                if ((x.room.get_tile(at + d).flags & world::tile_worm_grass) != 0) {
                    p.stuck_shake = int16_t(std::max<int32_t>(p.stuck_shake, 40));
                    x.vel(0) -= rw::to_vec2(d) * 2.f + custom::rnv(rng) * 6.f + vec2{0.f, 6.f};
                }
            }
            if (p.stuck_shake > 0) {
                --p.stuck_shake;
                stuck_behavior(x);
                return;
            }
            if (!p.hover_still) {
                bool flag3 = true;
                for (auto const& ww : x.wings)
                    flag3 = flag3 && (has_any_grip(ww) || ww.w.mode != wing_mode::climb);
                if (b.hanging_in_tentacle && flag3) {
                    ++b.release_gripping_tentacle;
                    if (b.release_gripping_tentacle > 5 && x.all_wings(wing_mode::climb))
                        release_grip(x, max_release(x));
                    else if (b.release_gripping_tentacle > 50)
                        release_grip(x, max_release(x));
                } else {
                    b.release_gripping_tentacle = 0;
                }
                bool flag4 = true;
                for (auto& ww : x.wings)
                    flag4 = flag4 && wing_space(x, ww);
                if (x.miros() && laser_active(x) && x.any_wing(wing_mode::climb)) {
                    if (b.time_since_last_takeoff >= 40)
                        take_off(x);
                    else
                        for (auto& ww : x.wings)
                            switch_mode(x, ww, wing_mode::fly);
                    b.dont_switch_modes_counter = 200;
                }
                ++b.time_since_last_takeoff;
                ivec2 const dt = conn->dest;
                int32_t const prox = x.proximity(dt);
                bool const sky_ahead = ray_trace_tiles_for_terrain(x.room, room_view::get_tile_position(x.pos(0)), room_view::get_tile_position(x.pos(0) + p.move_direction * 400.f));
                bool const land = prox >= 0 && prox <= (x.miros() ? 4 : 8) && fall_risk_close(x, dt) && x.all_wings(wing_mode::fly);
                bool const lift = (prox > 5 || prox < 0) && x.all_wings(wing_mode::climb) && x.vel(0).y > 4.f && p.move_direction.y > 0.f && sky_ahead && flag4;
                if (b.dont_switch_modes_counter > 0) {
                    --b.dont_switch_modes_counter;
                } else if (x.miros()) {
                    if (lift) {
                        take_off(x);
                        b.dont_switch_modes_counter = 200;
                    } else if ((prox > 4 || prox < 0) && x.any_wing(wing_mode::climb)) {
                        for (auto& ww : x.wings)
                            switch_mode(x, ww, wing_mode::fly);
                        b.dont_switch_modes_counter = 200;
                    } else if (land) {
                        for (auto& ww : x.wings)
                            switch_mode(x, ww, wing_mode::climb);
                        air_brake(x, 30);
                        b.dont_switch_modes_counter = 200;
                    }
                } else if (land) {
                    for (auto& ww : x.wings)
                        switch_mode(x, ww, wing_mode::climb);
                    air_brake(x, 30);
                    b.dont_switch_modes_counter = 200;
                } else if (lift) {
                    take_off(x);
                    b.dont_switch_modes_counter = 200;
                }
            }
            bool flag5 = true;
            for (auto const& ww : x.wings)
                flag5 = flag5 && !has_any_grip(ww);
            if (x.vel(0).y < -10.f && x.all_wings(wing_mode::climb) && flag5 && p.landing_brake < 1) {
                for (auto& ww : x.wings)
                    switch_mode(x, ww, wing_mode::fly);
                x.b.color_wave_delay = rng.range(10, 20);
                ++x.b.color_waves;
            }
            if (x.any_wing(wing_mode::fly))
                p.wing_flap_amplitude = mathf::clamp(p.wing_flap_amplitude + 0.033333335f, 0.f, 1.f);
            else if (!x.any_wing(wing_mode::fly) && x.all_wings(wing_mode::climb))
                p.wing_flap_amplitude = 0.f;
            else
                p.wing_flap_amplitude = mathf::clamp(p.wing_flap_amplitude + 0.0125f, 0.f, 0.5f);
            if (p.hover_still) {
                if (!p.last_hover_still || !custom::dist_less(x.pos(0), room_view::middle_of_tile(b.hover_pos), 60.f))
                    b.hover_pos = room_view::get_tile_position(x.pos(0));
                x.vel(1).y += 0.1f * num2;
                for (uint32_t k = 0; k < 4; ++k) {
                    x.vel(k) *= mathf::lerp(1.f, 0.9f, num2);
                    x.vel(k) += 0.6f * num2 * rw::clamp_magnitude(room_view::middle_of_tile(b.hover_pos) - x.pos(0), 10.f) / 10.f;
                }
            } else if (conn) {
                vec2 v = custom::dir_vec(x.pos(0), room_view::middle_of_tile(conn->dest));
                v = rw::lerp(v, rw::to_vec2(custom::clamp_at_one(conn->dest - conn->start)), 0.5f);
                if (x.air_borne()) {
                    if (x.room.inside(at.x, at.y) || v.y < 0.f) {
                        v.y *= 0.5f;
                        if (v.y < 0.f)
                            v.y = 0.f;
                    }
                    if (conn->dest.y > conn->start.y)
                        x.vel(1).y += 3.5f;
                    else if (conn->dest.y < conn->start.y)
                        p.wing_flap -= 0.014285714f;
                } else if (!x.room.inside(conn->dest.x, conn->dest.y)) {
                    if (conn->dest.y > x.room.height)
                        take_off(x);
                    else
                        for (auto& ww : x.wings)
                            switch_mode(x, ww, wing_mode::fly);
                }
                for (uint32_t k = 0; k < 4; ++k)
                    x.vel(k) += v * (x.air_borne() ? 0.6f : (x.king() ? 1.9f : 1.2f)) * num2;
                connection c2 = *conn;
                bool moved = false;
                for (int32_t k = 0; k < 3; ++k) {
                    auto const c3 = follow_path(x, c2.dest);
                    if (!c3)
                        break;
                    c2 = *c3;
                    moved = true;
                }
                if (!moved)
                    p.move_direction = rw::normalized(p.move_direction + v * 0.1f);
                else
                    p.move_direction = rw::normalized(p.move_direction + custom::dir_vec(x.pos(0), room_view::middle_of_tile(c2.dest)) * 0.5f);
            }
            bool flag7 = false;
            for (auto const& ww : x.wings)
                flag7 = flag7 || !has_any_grip(ww);
            if (!p.hover_still && !b.hanging_in_tentacle && flag7) {
                float num20 = 0.f;
                int32_t num21 = -1;
                for (uint32_t i = 0; i < 4; ++i) {
                    float const u = thruster_utility(x, i);
                    if (u > num20) {
                        num20 = u;
                        num21 = int32_t(i);
                    }
                }
                num20 *= p.jet_fuel;
                if (num20 > 0.05f && num21 >= 0)
                    thruster_activate(x, uint32_t(num21), 10 + int32_t(mathf::inverse_lerp(0.05f, 0.4f, num20) * 20.f));
            }
            if (p.snap_frames == 0) {
                if (x.ai.prey_in_tusk_charge_range || x.air_borne())
                    p.tusk_charge = mathf::clamp(p.tusk_charge + 0.025f, 0.f, 1.f);
                else
                    p.tusk_charge = mathf::clamp(p.tusk_charge - 0.011111111f, 0.f, 1.f);
            } else {
                vec2 const sp = snap_pos(x);
                if (p.snap_frames <= 21) {
                    x.vel(0) += custom::dir_vec(x.pos(0), sp) * 1.f;
                } else {
                    x.vel(1) -= custom::dir_vec(x.pos(1), sp) * 0.5f;
                    if (!x.air_borne())
                        for (uint32_t k = 0; k < 4; ++k)
                            x.vel(k) *= mathf::lerp(1.f, 0.2f, num2);
                }
                --p.snap_frames;
            }
            p.last_hover_still = p.hover_still;
            if (conn)
                b.last_connection = conn->dest;
        }

        // carried off over the top of the room: what it holds is done for
        // (like prey taken into a den: a slugcat is eaten, a creature is gone)
        void take_prey_to_sky(ctx& x) {
            auto& w = x.w();
            auto const t = x.grasped();
            if (!t || x.pos(0).y < x.room.pixel_height() + 200.f || !creature::live(x.state, t))
                return;
            creature::release_target(x.state, t, x.room.id);
            if (auto const pl = creature::player_of(x.state, t)) {
                items::release_all(x.state, pl);
                creature::kill(x.state, t, creature::template_of(x.state, x.c));
                w.player_set_eaten(pl, 1);
            } else {
                creature::kill(x.state, t, creature::template_of(x.state, x.c));
                x.state.defer(x.room.id, [t](sys::game_state& st) {
                    if (st.world.creature_is_valid(t))
                        creature::destroy(st, t);
                });
            }
            // it stays up there a while, eating
            x.ai.sky_counter = x.rng.range(800, 2400);
        }

        void set_up(ctx& x, vec2 at) {
            x.p.move_direction = vec2{0.f, 1.f};
            tentacle_reset(x, x.neck, at);
            tentacle_new_room(x, x.neck);
            for (auto& ww : x.wings) {
                tentacle_new_room(x, ww.tn);
                tentacle_reset(x, ww.tn, x.pos(ww.w.t.connected_chunk));
            }
            if (x.king())
                tusks_new_room(x);
            x.ai.king_tusk_shoot_pos = x.at();
            x.ai.time_in_room = 0;
            x.ai.idle_counter = 0;
            x.ai.sky_x = -1;
            x.ai.focus = dcon::creature_id{};
            x.b.last_connection = ivec2{-1000, -1000};
        }

        // AppendagePosition: the neck (0: the main chunk, the neck's chunks,
        // the head), a wing (its connected chunk, then its chunks)
        uint32_t appendage_positions(sys::game_state const& state, dcon::creature_id c, uint32_t app, vec2* out) {
            auto const& w = state.world;
            auto const d = creature::vulture_data(w, c);
            if (app == 0) {
                auto const& n = w.vulture_data_get_neck(d);
                out[0] = w.body_chunk_get_pos(creature::chunk(state, c, 0));
                for (uint32_t i = 0; i < neck_chunks; ++i)
                    out[1 + i] = n.c[i].pos;
                out[1 + neck_chunks] = w.body_chunk_get_pos(creature::chunk(state, c, head));
                return neck_chunks + 2;
            }
            uint32_t k = 0;
            for (auto const o : creature::parts(w, c)) {
                auto const part = w.part_ownership_get_part(o);
                if (w.creature_part_get_kind(part) != creature::part_kind::vulture_wing || w.creature_part_get_index(part) != app - 1)
                    continue;
                auto const id = dcon::vulture_wing_id{dcon::vulture_wing_id::value_base_t(w.creature_part_get_payload(part).vulture_wing.data)};
                auto const& wg = w.vulture_wing_get_t(id);
                out[0] = w.body_chunk_get_pos(creature::chunk(state, c, wg.t.connected_chunk));
                for (uint32_t i = 0; i < wg.chunk_count; ++i)
                    out[1 + i] = wg.c[i].pos;
                k = wg.chunk_count + 1u;
                break;
            }
            return k;
        }

        // the closest distance between the segments a-b and c-d, and where
        // along each (s on a-b, t on c-d)
        float segment_distance(vec2 a, vec2 b, vec2 c, vec2 d, float& s, float& t) {
            vec2 const d1 = b - a, d2 = d - c, r = a - c;
            float const aa = rw::dot(d1, d1), ee = rw::dot(d2, d2), f = rw::dot(d2, r);
            if (aa <= 1e-6f && ee <= 1e-6f) {
                s = t = 0.f;
                return rw::distance(a, c);
            }
            if (aa <= 1e-6f) {
                s = 0.f;
                t = mathf::clamp01(f / ee);
            } else {
                float const cc = rw::dot(d1, r);
                if (ee <= 1e-6f) {
                    t = 0.f;
                    s = mathf::clamp01(-cc / aa);
                } else {
                    float const bb = rw::dot(d1, d2);
                    float const den = aa * ee - bb * bb;
                    s = den != 0.f ? mathf::clamp01((bb * f - cc * ee) / den) : 0.f;
                    t = (bb * s + f) / ee;
                    if (t < 0.f) {
                        t = 0.f;
                        s = mathf::clamp01(-cc / aa);
                    } else if (t > 1.f) {
                        t = 1.f;
                        s = mathf::clamp01((bb - cc) / aa);
                    }
                }
            }
            return rw::distance(a + d1 * s, c + d2 * t);
        }

        // ApplyForceOnAppendage
        void apply_force_on_appendage(ctx& x, creature::hit_part const& part, vec2 momentum) {
            float const dn = mathf::clamp01(part.distance_to_next);
            int32_t const ps = part.prev_segment;
            if (part.tentacle > 0) {
                uint32_t const i = uint32_t(part.tentacle - 1);
                if (i >= x.wings.size())
                    return;
                auto& ww = x.wings[i];
                uint32_t const n = ww.w.chunk_count;
                if (ps > 0 && uint32_t(ps - 1) < n) {
                    ww.w.c[ps - 1].pos += momentum / 0.04f * (1.f - dn);
                    ww.w.c[ps - 1].vel += momentum / 0.04f * (1.f - dn);
                } else {
                    uint32_t const cc = ww.w.t.connected_chunk;
                    x.pos(cc) += momentum / x.mass(cc) * (1.f - dn);
                    x.vel(cc) += momentum / x.mass(cc) * (1.f - dn);
                }
                if (ps >= 0 && uint32_t(ps) < n) {
                    ww.w.c[ps].pos += momentum / 0.04f * dn;
                    ww.w.c[ps].vel += momentum / 0.04f * dn;
                }
                return;
            }
            if (ps > 0 && uint32_t(ps - 1) < neck_chunks) {
                x.neck.c[ps - 1].pos += momentum / 0.1f * (1.f - dn);
                x.neck.c[ps - 1].vel += momentum / 0.05f * (1.f - dn);
            } else {
                x.pos(0) += momentum / x.mass(0) * (1.f - dn);
                x.vel(0) += momentum / x.mass(0) * (1.f - dn);
            }
            if (ps >= 0 && ps < int32_t(neck_chunks) - 1) {
                x.neck.c[ps].pos += momentum / 0.1f * dn;
                x.neck.c[ps].vel += momentum / 0.05f * dn;
            } else {
                x.pos(head) += momentum / x.mass(head) * dn;
                x.vel(head) += momentum / x.mass(head) * dn;
            }
        }

        // Vulture.DropMask: no VultureMask item here (see DIVERGENCES): the
        // mask is gone, and with it the vulture's liking for the slugcats
        void drop_mask(ctx& x, vec2) {
            if (!x.p.mask)
                return;
            x.p.mask = 0;
        }

        // Vulture.SpearStick: false when the mask turns the weapon (it comes
        // at the face head on)
        bool spear_stick(ctx& x, uint32_t chunk_i, vec2 dir) {
            if (x.miros())
                return true;
            return chunk_i != head || x.dead() || !x.p.mask || rw::dot(rw::normalized(dir), custom::dir_vec(x.neck.tip().pos, x.pos(head))) >= -0.88f;
        }

        // rooms with open sky over them and a den with room in front
        // (an OFFSCREEN vulture's home here)
        bool sky_den(sys::game_state const& state, room_view const& room, uint8_t node) {
            auto const e = dens::den_entrance(state, room.id, node);
            if (!e)
                return false;
            ivec2 const front = *e + room.shortcut_entrance_hole_direction(*e) * 2;
            return room.inside(front.x, front.y) && pathing::terrain_proximity(room, front, 3) >= 2;
        }
    }

    //
    // The kind's hooks
    //
    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        ctx x{state, c, room};
        load(x);
        check_flip(x);
        // Creature.Update + PhysicalObject.Update
        creature::stun_countdown(state, c);
        physics::update_object(state, room, body(state, c), nullptr, nullptr);
        // (the terrain impact sounds have no sample here)
        float const health = w.creature_get_health(c);
        if (!w.creature_get_dead(c) && health < 0.f && x.rng.value() < -health && x.rng.value() < 0.025f)
            creature::die(state, c);
        // the miros vulture's laser
        if (x.miros() && x.p.laser_counter > 0) {
            --x.p.laser_counter;
            if (x.p.laser_counter == 10 && !x.dead()) {
                laser_explosion(x);
                --x.p.laser_counter;
            }
            if (x.dead() || !x.mostly_consious() || shadow_mode(x))
                x.p.laser_counter = 0;
        }
        x.b.hanging_in_tentacle = 0;
        for (auto& ww : x.wings)
            wing_update(x, ww);
        if (x.b.hanging_in_tentacle) {
            x.b.cant_find_new_grip_counter += 2;
            if (x.b.cant_find_new_grip_counter > (x.miros() ? 200 : 400))
                for (auto& ww : x.wings)
                    if (has_any_grip(ww))
                        release_grip(x, ww);
        } else if (x.b.cant_find_new_grip_counter > 0) {
            --x.b.cant_find_new_grip_counter;
        }
        if (w.creature_get_entering_shortcut(c).x < 0)
            update_neck(x);
        if (x.p.landing_brake > 0) {
            --x.p.landing_brake;
            for (uint32_t l = 0; l < chunk_count; ++l)
                x.vel(l) *= 0.7f;
            x.vel(1) += rw::clamp_magnitude(x.b.landing_brake_pos - x.pos(1), 40.f) / 20.f;
        }
        bool none = true;
        for (uint32_t m = 0; m < 4; ++m) {
            thruster_update(x, m);
            none = none && !thruster_active(x, m);
        }
        if (none)
            set_jet_fuel(x, x.p.jet_fuel + 0.008333334f);
        if (x.rng.value() * 0.75f > w.creature_get_health(c))
            stun(x, 10);
        if (x.grasped())
            carry(x);
        if (x.awake())
            act(x);
        if (x.king())
            tusks_update(x);
        take_prey_to_sky(x);
        store(x);
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 pos, variant v) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::vulture);
        rw::rng rng{state.random.next() | 1u};
        int32_t const seed = state.spawn_seed >= 0 ? state.spawn_seed : int32_t(rng.next() & 0x7FFFFFFF);
        bool const king = v == variant::king, miros = v == variant::miros;
        float const num = king ? 1.4f : 1.f;
        physics::object_desc desc;
        desc.air_friction = 0.99f;
        desc.gravity = 0.9f;
        desc.bounce = 0.1f;
        desc.surface_friction = 0.35f;
        desc.water_friction = 0.9f;
        desc.buoyancy = 0.92f;
        auto const o = physics::create_object(state, room, desc);
        // the body laid out as the connections hold it: 1 the back at `pos`,
        // 0 the front over it, 2 and 3 the wings' bases, the head up front
        vec2 const offs[5] = {vec2{0.f, 26.f}, vec2{0.f, 0.f}, vec2{-20.f, 10.f}, vec2{20.f, 10.f}, vec2{0.f, 26.f + (king ? 70.f : 60.f) * 0.6f}};
        dcon::body_chunk_id ks[chunk_count];
        for (uint32_t i = 0; i < 4; ++i)
            ks[i] = physics::add_chunk(state, o, pos + offs[i], 9.5f, miros ? 1.8f : 1.2f * num);
        ks[head] = physics::add_chunk(state, o, pos + offs[head], 6.5f, 0.3f * num);
        for (auto const k : ks)
            w.body_chunk_set_restrict_in_room_range(k, 2400.f);
        constexpr float num2 = 40.f, num3 = 26.f, num4 = 10.f;
        using physics::connection_type;
        physics::add_connection(state, o, ks[0], ks[1], num3, connection_type::normal, 1.f, 0.5f);
        physics::add_connection(state, o, ks[2], ks[3], num2, connection_type::normal, 1.f, 0.5f);
        physics::add_connection(state, o, ks[2], ks[1], std::sqrt(num4 * num4 + (num2 / 2.f) * (num2 / 2.f)), connection_type::normal, 1.f, 0.5f);
        physics::add_connection(state, o, ks[1], ks[3], std::sqrt(num4 * num4 + (num2 / 2.f) * (num2 / 2.f)), connection_type::normal, 1.f, 0.5f);
        physics::add_connection(state, o, ks[2], ks[0], std::sqrt((num3 - num4) * (num3 - num4) + (num2 / 2.f) * (num2 / 2.f)), connection_type::normal, 1.f, 0.5f);
        physics::add_connection(state, o, ks[3], ks[0], std::sqrt((num3 - num4) * (num3 - num4) + (num2 / 2.f) * (num2 / 2.f)), connection_type::normal, 1.f, 0.5f);
        physics::add_connection(state, o, ks[0], ks[head], king ? 70.f : 60.f, connection_type::pull, 0.6f, 0.f);
        w.try_create_creature_body(c, o);
        physics::set_go_through_floors(state, o, true);
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_set_behavior(c, uint8_t(behavior::idle));
        w.creature_set_current_utility(c, 0.f);
        w.creature_set_destination(c, room_view::get_tile_position(pos));
        auto& p = w.creature_get_payload(c).vulture;
        uint16_t const data = p.data;
        p = creature::vulture_payload{};
        p.data = data;
        p.variant = v;
        p.mask = miros ? 0 : 1;
        p.jet_fuel = 1.f;
        p.wing_flap_amplitude = 1.f;
        p.move_direction = vec2{0.f, 1.f};
        uint32_t const wings = miros ? 4u : 2u;
        p.wing_count = uint8_t(wings);
        auto const d = creature::vulture_data(w, c);
        w.vulture_data_set_seed(d, seed);
        w.vulture_data_set_body(d, body_state{});
        w.vulture_data_set_ai(d, ai_state{});
        w.vulture_data_set_tusks(d, king_tusks{});
        neck_state n{};
        n.t.ideal_length = (king ? 6.f : 5.f) * 20.f;
        n.t.connected_chunk = 0;
        for (uint32_t k = 0; k < neck_chunks; ++k) {
            n.c[k].t_pos = float(k + 1) / float(neck_chunks);
            n.c[k].rad = king ? 6.f : 5.f;
        }
        n.c[neck_chunks - 1].rad = 7.f;
        w.vulture_data_set_neck(d, n);
        for (uint32_t j = 0; j < wings; ++j) {
            auto const part = creature::add_part(state, c, creature::part_kind::vulture_wing, uint8_t(j));
            auto const l = w.create_vulture_wing();
            w.creature_part_get_payload(part).vulture_wing = creature::vulture_wing_payload{uint16_t(l.index())};
            wing wg{};
            wg.t.ideal_length = (king ? 9.f : 7.f) * 20.f;
            wg.t.connected_chunk = uint8_t(2 + j % 2);
            wg.chunk_count = uint8_t(king ? 10 : 8);
            for (uint32_t i = 0; i < wg.chunk_count; ++i) {
                wg.c[i] = tentacle_chunk{};
                wg.c[i].t_pos = float(i + 1) / float(wg.chunk_count);
                wg.c[i].rad = 5.f;
            }
            wg.mode = wing_mode::climb;
            w.vulture_wing_set_t(l, wg);
            w.vulture_wing_get_segments(l).resize(0);
            w.vulture_wing_get_grab_path(l).resize(0);
        }
        tracker::clear(state, c);
        w.creature_set_random(c, rng);
        {
            auto const view = world::get_room_view(state, room);
            ctx x{state, c, view};
            load(x);
            set_up(x, x.pos(0));
            store(x);
        }
        vulture_graphics::init(state, c);
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        creature::release_all(state, c, creature::room_of(state, c));
        creature::release_target(state, c);
        creature::erase(state, c);
    }

    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) {
        // what the beak holds is kept by the carry
        return !creature::holds(state.world, c, t) && !creature::holds(state.world, t, c);
    }

    void collide(sys::game_state& state, dcon::creature_id c, room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j) {
        auto& p = state.world.creature_get_payload(c).vulture;
        if (p.variant == variant::miros || i != head || p.snap_frames <= 0 || p.snap_frames > 21 || creature::grasped(state.world, c, 0))
            return;
        if (!creature::consious(state, c) || j >= creature::chunk_count_of(state, t))
            return;
        ctx x{state, c, room};
        vec2 const mom = x.vel(head) * 2.f;
        creature::tmpl const me = creature::template_of(state, c);
        if (only_hurt_dont_grab(x, t)) {
            creature::violence_on(state, t, j, mom, 1.1f, 30.f, creature::damage_type::bite, me);
        } else {
            grab_in_beak(x, t, j);
            x.ai.focus = dcon::creature_id{};
            creature::violence_on(state, t, j, mom, 0.4f, 20.f, creature::damage_type::bite, me);
        }
        p.snap_frames = 0;
    }

    void violence(sys::game_state& state, dcon::creature_id c, creature::hit_part const& part, vec2 momentum, float damage, float stun_bonus, creature::damage_type type) {
        auto& w = state.world;
        auto const room = creature::room_of(state, c);
        if (!room)
            return;
        auto const view = world::get_room_view(state, room);
        ctx x{state, c, view};
        load(x);
        bool const on_app = part.chunk < 0 && part.tentacle >= 0;
        uint32_t const k = part.chunk >= 0 ? std::min<uint32_t>(uint32_t(part.chunk), chunk_count - 1) : 0u;
        bool const weapon = type == creature::damage_type::stab || type == creature::damage_type::blunt;
        if (!x.miros())
            x.ai.disencouraged += (damage / 2.f + stun_bonus / 460.f) * (x.king() ? 0.3f : 1.f) * (state.story_slugcat == player::slugcat_name::yellow ? 1.5f : 1.f);
        if (!on_app && k == head) {
            stun_bonus += 20.f + 20.f * damage;
            if (damage > 0.1f || w.creature_get_stun(c) > 10)
                x.p.snap_frames = 0;
            // (only a thrown weapon carries a direction: SpearStick)
            bool const deflected = weapon && rw::sqr_magnitude(momentum) > 0.f && !spear_stick(x, k, momentum);
            if (x.king() && type == creature::damage_type::stab && x.rng.value() < (x.dead() ? 0.2f : 0.8f) && rw::sqr_magnitude(momentum) > 0.f
                && tusks_hit_by_spear(x, momentum)) {
                x.vel(head) += momentum * 0.8f;
                damage *= 0.1f;
            } else if (!deflected) {
                if (!x.miros() && x.p.mask && damage > 0.9f)
                    drop_mask(x, momentum / 5.f + custom::rnv(x.rng) * 7.f * x.rng.value());
                damage *= 1.5f;
            } else {
                // the mask turns it: sparks
                if (x.viewed && (damage > 0.1f || stun_bonus > 30.f)) {
                    auto& g = state.runtime(room).cosmetic;
                    vec2 const at = x.pos(head);
                    particles::spawn(state, room, particles::kind::flash, at, vec2{0.f, 0.f}, 12.f, 0.15f, vec3{1.f, 1.f, 1.f});
                    for (int32_t i = 0; i < 3 + int32_t(mathf::min(damage * 3.f, 9.f)); ++i)
                        particles::spawn(state, room, particles::kind::spark, at, custom::rnv(g) * g.value() * 12.f, 1.5f, 0.3f, vec3{1.f, 1.f, 1.f});
                }
                x.vel(head) += momentum * 0.8f;
                damage *= 0.1f;
            }
        }
        if (on_app) {
            if (part.tentacle == 0 && type != creature::damage_type::blunt) {
                damage *= 2.f;
            } else if (part.tentacle > 0 && (!x.miros() || type != creature::damage_type::explosion)) {
                uint32_t const wi = uint32_t(part.tentacle - 1);
                if (wi < x.wings.size())
                    wing_damage(x, x.wings[wi], damage, stun_bonus);
                damage /= 10.f;
                stun_bonus /= 20.f;
            }
        }
        if (x.miros() && !x.dead() && !x.grasped() && type != creature::damage_type::explosion)
            fire_laser(x);
        // Creature.Violence
        if (on_app) {
            apply_force_on_appendage(x, part, momentum);
        } else {
            x.vel(k) += rw::clamp_magnitude(momentum / x.mass(k), 10.f);
        }
        auto const& tp = params(x.v);
        float const dmg = damage / tp.damage_resistance;
        float st = (damage * 30.f + stun_bonus) / tp.stun_resistance;
        float& health = w.creature_get_health(c);
        st *= 1.5f + mathf::inverse_lerp(0.5f, 0.f, health) * x.rng.value();
        stun(x, int32_t(st));
        health -= dmg;
        store(x);
        // quickDeath
        if (!w.creature_get_dead(c) && (x.rng.value() < -health || health < -1.f || (health < 0.f && x.rng.value() < 0.33f)))
            creature::die(state, c);
    }

    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t) {
        auto& w = state.world;
        if (creature::holds(w, c, t))
            creature::release_grasp(state, c, uint32_t(creature::grasp_on(w, c, t)));
        tusks_release(state, c, t);
        auto& b = w.vulture_data_get_body(creature::vulture_data(w, c));
        if (b.snap_at == t)
            b.snap_at = dcon::creature_id{};
        auto& ai = w.vulture_data_get_ai(creature::vulture_data(w, c));
        if (ai.focus == t)
            ai.focus = dcon::creature_id{};
    }

    void died(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        // Vulture.Die: surfaceFriction 0.3
        w.physical_object_set_surface_friction(body(state, c), 0.3f);
        w.creature_get_payload(c).vulture.laser_counter = 0;
    }

    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const view = world::get_room_view(state, room);
        ivec2 const t = room_view::get_tile_position(at);
        vec2 const a = rw::to_vec2(view.shortcut_entrance_hole_direction(t));
        auto& rng = w.creature_get_random(c);
        for (uint32_t i = 0; i < chunk_count; ++i) {
            auto const k = chunk(state, c, i);
            w.body_chunk_set_pos(k, room_view::middle_of_tile(t) - a * (i == 1 ? 10.f : 5.f) + custom::deg_to_vec(rng.value() * 360.f));
            w.body_chunk_set_last_pos(k, room_view::middle_of_tile(t));
            w.body_chunk_set_last_last_pos(k, room_view::middle_of_tile(t));
            w.body_chunk_set_vel(k, a * 5.f);
        }
        ctx x{state, c, view};
        load(x);
        set_up(x, x.pos(0));
        store(x);
        w.creature_set_shortcut_delay(c, 80);
    }

    std::string describe(sys::game_state const& state, dcon::creature_id c) {
        auto const& w = state.world;
        auto const& p = w.creature_get_payload(c).vulture;
        vec2 const pos = w.body_chunk_get_pos(chunk(state, c, 0));
        auto const held = creature::grasped(w, c, 0);
        uint32_t flying = 0;
        for (auto const o : creature::parts(w, c)) {
            auto const part = w.part_ownership_get_part(o);
            if (w.creature_part_get_kind(part) != creature::part_kind::vulture_wing)
                continue;
            auto const id = dcon::vulture_wing_id{dcon::vulture_wing_id::value_base_t(w.creature_part_get_payload(part).vulture_wing.data)};
            flying += w.vulture_wing_get_t(id).mode == wing_mode::fly ? 1 : 0;
        }
        return std::string{to_string(p.variant)} + " at " + std::to_string(int(pos.x)) + "," + std::to_string(int(pos.y)) + " behavior "
            + std::to_string(int(w.creature_get_behavior(c))) + (flying == p.wing_count ? " flying" : flying > 0 ? " landing" : " climbing")
            + (p.variant != variant::miros ? (p.mask ? " masked" : " unmasked") : "") + (p.laser_counter > 0 ? " laser" : "")
            + " fuel " + std::to_string(int(p.jet_fuel * 100.f)) + "%"
            + (held ? std::string{" holding a "} + creature::to_string(w.creature_get_type(held)) : std::string{});
    }

    bool spawn_spot(room_view const& room, ivec2 tile) {
        // open air with room for the wings
        return room.inside(tile.x, tile.y) && pathing::accessible(room, tile, flyer) && !room.get_tile(tile).solid() && pathing::terrain_proximity(room, tile, 4) >= 4;
    }

    bool trace_appendages(sys::game_state const& state, dcon::room_id room, vec2 from, vec2 to, dcon::creature_id thrown_by, dcon::creature_id& hit,
        creature::hit_part& part, float& at) {
        auto const& w = state.world;
        float best = 2.f;
        rooms::for_each_creature(state, room, creature::type::vulture, [&](dcon::creature_id c) {
            if (c == thrown_by || w.creature_get_away(c) != 0)
                return;
            auto const& p = w.creature_get_payload(c).vulture;
            vec2 pts[max_wing_chunks + 2];
            for (uint32_t app = 0; app <= p.wing_count; ++app) {
                uint32_t const n = appendage_positions(state, c, app, pts);
                for (uint32_t s = 0; s + 1 < n; ++s) {
                    float sa = 0.f, sb = 0.f;
                    // the neck's chunks are 5 to 7 thick, a wing's its contour (~6)
                    float const rad = app == 0 ? 6.f : 6.f;
                    if (segment_distance(from, to, pts[s], pts[s + 1], sa, sb) < rad && sa < best) {
                        best = sa;
                        hit = c;
                        part = creature::hit_part{-1, int32_t(app), int32_t(s), sb};
                    }
                }
            }
        });
        at = best;
        return best <= 1.f;
    }

    variant variant_of(sys::game_state const& state, dcon::creature_id c) noexcept { return state.world.creature_get_payload(c).vulture.variant; }

    bool has_mask(sys::game_state const& state, dcon::creature_id c) noexcept { return state.world.creature_get_payload(c).vulture.mask != 0; }

    std::optional<std::pair<dcon::room_id, uint8_t>> offscreen_den(sys::game_state const& state, dcon::region_id region, int32_t seed) {
        auto const& w = state.world;
        std::vector<std::pair<dcon::room_id, uint8_t>> found;
        w.for_each_room([&](dcon::room_id r) {
            if (w.room_get_region_from_region_membership(r) != region || w.room_get_is_shelter(r) || w.room_get_is_gate(r))
                return;
            auto const view = world::get_room_view(state, r);
            if (view.width <= 0 || view.height <= 0 || sky_column(view, view.width / 2) < 0)
                return;
            for (uint32_t node = 0; node < 64; ++node)
                if (sky_den(state, view, uint8_t(node)))
                    found.push_back({r, uint8_t(node)});
        });
        if (found.empty())
            return std::nullopt;
        uint32_t const h = uint32_t(seed) * 2654435761u;
        return found[h % uint32_t(found.size())];
    }
}
