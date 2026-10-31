#include <cmath>
#include <vector>

#include "outposts.hpp"
#include "body_parts.hpp"
#include "items.hpp"
#include "physics.hpp"

namespace outposts {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // UnityEngine.Vector3 helpers
        float magnitude3(vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
        float distance3(vec3 a, vec3 b) { return magnitude3(a - b); }
        // Vector3.normalized: zero below 1e-5
        vec3 normalized3(vec3 v) {
            float const m = magnitude3(v);
            return m > 1e-5f ? v / m : vec3{0.f, 0.f, 0.f};
        }
        vec3 lerp3(vec3 a, vec3 b, float t) { return a + (b - a) * mathf::clamp01(t); }

        //
        // DeerGraphics.Antlers, rolled from the outpost's skullSeed
        //
        struct gen_values {
            float circumference_tend, goal_tend, random_tend;
            float circumference_tend_change, goal_tend_change, random_tend_change;
            float attract_rad;
        };
        struct part {
            std::vector<vec3> positions;
            std::vector<float> rads;
            int32_t last_branching_segment = 2;
            std::vector<vec3> ind[2];
        };

        // Antlers.GenerateValues
        gen_values generate_values(rw::unity_random& r, float rad) {
            gen_values g;
            g.attract_rad = rad * mathf::lerp(0.7f, 1.f, r.value());
            g.circumference_tend = mathf::lerp(0.f, 0.01f, r.value());
            g.goal_tend = mathf::lerp(0.f, 0.02f, r.value());
            g.random_tend = mathf::lerp(0.1f, 0.4f, r.value());
            g.circumference_tend_change = mathf::lerp(-1.f, 1.f, r.value()) * 0.01f * 0.5f;
            g.goal_tend_change = mathf::lerp(-1.f, 1.f, r.value()) * 0.02f * 0.5f;
            g.random_tend_change = mathf::lerp(-1.f, 1.f, r.value()) * 0.1f * 0.5f;
            return g;
        }

        // Antlers.GeneratePart: false when it would be too short (null)
        bool generate_part(rw::unity_random& r, float rad, bool spine, float start_rad, vec3 const* before_start, vec3 start, vec3 goal, vec3 init_dir,
            gen_values const& g, part& out) {
            std::vector<vec3> list;
            std::vector<float> list2;
            if (before_start != nullptr) {
                list.push_back(*before_start);
                list2.push_back(start_rad * 0.5f);
            }
            vec3 v = start;
            vec3 a = init_dir;
            float num = g.circumference_tend;
            float num2 = g.goal_tend;
            float num3 = g.random_tend;
            constexpr float num4 = 10.f;
            int32_t const num5 = int32_t(std::floor(distance3(start, goal) / num4));
            if (num5 <= 2)
                return false;
            vec3 const centre{0.f, rad * -0.5f, 0.f};
            for (int32_t i = 0; i < num5; ++i) {
                float const f = float(i) / float(num5 - 1);
                list.push_back(v);
                float num6 = mathf::lerp(start_rad, 1.2f, mathf::pow(f, 0.5f));
                num6 = mathf::lerp(num6, custom::lerp_map(distance3(v, centre), 0.f, rad * 1.5f, 6.f, 1.2f), 0.5f);
                list2.push_back(num6);
                v += a * num4;
                if (spine)
                    v.z -= 11.f * std::sin(float(i) / float(num5 - 1) * mathf::pi);
                if (v.x < 15.f)
                    v.x = 15.f;
                v = lerp3(v, goal, mathf::pow(f, 4.f));
                vec3 const sphere = r.on_unit_sphere();
                float const m = magnitude3(v);
                a = normalized3(a + sphere * num3 + normalized3(v) * (g.attract_rad - m) * (m > rad ? mathf::max(num, 0.2f) : num) + normalized3(goal - v) * num2);
                num = mathf::clamp(num + g.circumference_tend_change, 0.f, 0.5f);
                num2 = mathf::clamp(num2 + g.goal_tend_change, 0.f, 0.5f);
                num3 = mathf::clamp(num3 + g.random_tend_change, 0.f, 0.5f);
            }
            size_t const last = list.size() - 1;
            vec3 v2 = lerp3(list[last], list[last - 1], 0.75f);
            v2 = lerp3(v2, list[last] - normalized3(list[last] - list[last - 1]) * 3.f, 1.f);
            list.insert(list.begin() + std::ptrdiff_t(last), v2);
            list2.insert(list2.begin() + std::ptrdiff_t(list2.size() - 1),
                mathf::lerp(start_rad, custom::lerp_map(distance3(goal, centre), 0.f, rad * 1.5f, 7.f, 4.f), 0.5f));
            list2.back() *= 0.15f;
            out.positions = std::move(list);
            out.rads = std::move(list2);
            return true;
        }

        // Antlers.Part.GenerateInds
        void generate_inds(rw::unity_random& r, part& p) {
            for (int i = 0; i < 2; ++i) {
                p.ind[i].assign(p.positions.size(), vec3{0.f, 0.f, 0.f});
                vec3 const sphere = r.on_unit_sphere();
                vec3 v = sphere * 11.f * mathf::pow(r.value(), 0.75f);
                for (size_t j = size_t(p.last_branching_segment + 2); j < p.positions.size(); ++j) {
                    p.ind[i][j] += v;
                    v *= 1.1f;
                    vec3 const s2 = r.on_unit_sphere();
                    v += s2 * 2.f * r.value();
                }
            }
        }

        // the Antlers constructor
        std::vector<part> generate_antlers(rw::unity_random& r, float rad) {
            gen_values g = generate_values(r, rad);
            std::vector<part> list;
            part spine;
            vec2 const goal2 = custom::deg_to_vec(mathf::lerp(10.f, 45.f, r.value())) * rad;
            float const ix = mathf::lerp(0.4f, 1.f, r.value());
            float const iz = mathf::lerp(-1.f, 1.f, r.value());
            if (!generate_part(r, rad, true, 5.f, nullptr, vec3{0.f, -rad, 0.f}, vec3{goal2.x, goal2.y, 0.f}, normalized3(vec3{ix, 1.f, iz}), g, spine))
                return list;
            g.goal_tend = mathf::lerp(g.goal_tend, 0.2f, 0.5f);
            int32_t num = int32_t(spine.positions.size());
            list.push_back(std::move(spine));
            constexpr int32_t num2 = 30;
            vec3 arr[num2];
            vec3 v = r.on_unit_sphere();
            for (int32_t i = 0; i < num2; ++i) {
                arr[i] = v;
                v = normalized3(-v + r.on_unit_sphere());
                if (arr[i].x < 0.f)
                    arr[i].x *= -1.f;
            }
            arr[0].z -= 1.f;
            arr[0] = normalized3(arr[0]);
            arr[1].z += 1.f;
            arr[1] = normalized3(arr[1]);
            for (int32_t n6 = 0; n6 < num2 && num < 80; ++n6) {
                arr[n6] *= rad * mathf::lerp(0.2f, 2.f, mathf::pow(r.value(), 0.45f));
                float best = std::numeric_limits<float>::max();
                int32_t seg = 0;
                size_t index = 0;
                for (size_t j = 0; j < list.size(); ++j) {
                    auto const& ps = list[j].positions;
                    for (size_t k = 0; k + 1 < ps.size(); ++k) {
                        float const d = distance3(ps[k], arr[n6]);
                        float d2 = mathf::lerp(d, ps[k].y + rad, mathf::inverse_lerp(0.f, rad * 2.5f, d) * 0.15f);
                        d2 *= 2.f - mathf::pow(std::sin(mathf::pow(float(k) / float(ps.size() - 2), 1.f) * mathf::pi), 0.5f);
                        if (d2 < best) {
                            seg = int32_t(k);
                            best = d2;
                            index = j;
                        }
                    }
                }
                auto& host = list[index];
                seg = std::max(1, seg - int32_t(std::floor(distance3(host.positions[size_t(seg)], arr[n6]) / 30.f)));
                host.last_branching_segment = seg;
                vec3 const start = host.positions[size_t(seg)];
                vec3 const dir = normalized3(host.positions[size_t(seg)] - host.positions[size_t(seg - 1)]);
                vec3 const before = host.positions[size_t(seg - 1)];
                float const start_rad = host.rads[size_t(seg)];
                part p2;
                if (generate_part(r, rad, false, start_rad, &before, start, arr[n6], dir, g, p2)) {
                    num += int32_t(p2.positions.size());
                    list.push_back(std::move(p2));
                }
            }
            for (auto& p : list)
                generate_inds(r, p);
            return list;
        }

        // an abstract object waiting to be realised
        struct pending {
            world::item_type type;
            vec2 pos;
            int8_t charge = -1;
            bool pearl = false;
        };
        dcon::item_id realize(sys::game_state& state, dcon::room_id room, pending const& p) {
            auto const it = items::create_on_tile(state, room, p.pos, p.type);
            if (p.charge >= 0)
                state.world.item_set_charge(it, p.charge);
            return it;
        }

        // the ScavengerOutpost constructor (on the room's first visit)
        void spawn_outpost(sys::game_state& state, dcon::room_id room, dcon::placed_thing_id t) {
            auto& w = state.world;
            auto const view = world::get_room_view(state, room);
            vec2 const pos = w.placed_thing_get_pos(t);
            vec2 const handle = w.placed_thing_get_handle(t);
            float const rad = rw::magnitude(handle);
            int32_t const skull_seed = int32_t(w.placed_thing_get_handle2(t).x);
            int32_t const pearls_seed = int32_t(w.placed_thing_get_handle2(t).y);
            float const direction = w.placed_thing_get_extra(t);

            // scavAccessibleTiles (the y loop runs up to the x bound, like vanilla)
            ivec2 const tp = room_view::get_tile_position(pos);
            int32_t const n = int32_t(std::nearbyint(rad / 20.f));
            std::vector<ivec2> tiles;
            for (int32_t j = tp.x - n; j <= tp.x + n; ++j)
                for (int32_t k = tp.y - n; k <= tp.x + n; ++k)
                    if (!view.get_tile(j, k).solid() && view.get_tile(j, k - 1).solid())
                        tiles.push_back(ivec2{j, k});
            std::vector<pending> spears;
            if (!tiles.empty())
                for (int32_t l = state.room_random(room).range(12, 24); l >= 0; --l) {
                    ivec2 const at = tiles[size_t(state.room_random(room).range(0, int32_t(tiles.size())))];
                    spears.push_back(pending{world::item_type::spear, room_view::middle_of_tile(at)});
                }

            auto const o = w.create_outpost();
            w.outpost_set_pos(o, pos);
            w.outpost_set_handle(o, handle);
            float const flip = mathf::lerp(-0.8f, 0.8f, direction);
            w.outpost_set_antler_flip(o, flip);
            w.try_create_outpost_location(o, room);
            {
                // Random.InitState(skullSeed): the antlers and the paint job
                rw::unity_random r{skull_seed};
                auto const parts = generate_antlers(r, antler_rad);
                auto pool = w.outpost_get_antlers(o);
                for (size_t i = 0; i < parts.size(); ++i)
                    for (size_t k = 0; k < parts[i].positions.size(); ++k) {
                        items::antler_point ap;
                        ap.pos = parts[i].positions[k];
                        ap.rad = parts[i].rads[k];
                        ap.ind[0] = parts[i].ind[0][k];
                        ap.ind[1] = parts[i].ind[1][k];
                        ap.part = uint8_t(i);
                        pool.push_back(ap);
                    }
                w.outpost_set_antler_parts(o, uint8_t(parts.size()));
                w.outpost_set_antler_pos(o, pos + handle + rw::normalized(handle) * antler_rad);
                w.outpost_set_paint_job(o, r.value() < 0.5f ? 1 : 0);
            }
            std::vector<pending> pearls;
            std::vector<std::pair<dcon::pearl_string_id, float>> strung;
            {
                // Random.InitState(pearlsSeed): the pearl strings
                rw::unity_random r{pearls_seed};
                int32_t const count = r.range(5, 15);
                int32_t const parts = w.outpost_get_antler_parts(o);
                auto const points = w.outpost_get_antlers(o);
                for (int32_t s = 0; s < count && parts > 0; ++s) {
                    float const length = 20.f + mathf::lerp(20.f, 150.f, r.value()) * custom::lerp_map(float(count), 5.f, 15.f, 1.f, 0.1f);
                    auto const ps = w.create_pearl_string();
                    w.try_create_string_outpost(ps, o);
                    int32_t const on_part = r.range(0, parts);
                    int32_t last = 0;
                    for (uint32_t k = 0; k < points.size(); ++k)
                        if (points.at(k).part == on_part)
                            ++last;
                    w.pearl_string_set_on_part(ps, uint8_t(on_part));
                    w.pearl_string_set_on_pos(ps, uint8_t(last - 1));
                    w.pearl_string_set_on_side(ps, int8_t(r.value() < 0.5f ? -1 : 1));
                    vec2 at = attached_pos(state, ps);
                    float num = mathf::lerp(40.f, 70.f, r.value());
                    float num2 = 0.f;
                    for (int32_t num3 = 0; float(num3) < length; ++num3) {
                        if (r.value() < 0.5f || num3 == 1)
                            num = mathf::lerp(5.f, 40.f, r.value());
                        num2 += num;
                        at.y -= num;
                        pearls.push_back(pending{world::item_type::data_pearl, at, -1, true});
                        strung.push_back({ps, num});
                        if (num2 > length)
                            break;
                    }
                }
            }
            // the realised objects in the order they were added: the toll
            // spears, then the strung pearls
            for (auto const& p : spears)
                realize(state, room, p);
            w.outpost_set_toll_spears(o, uint16_t(spears.size()));
            for (size_t i = 0; i < pearls.size(); ++i) {
                auto const it = realize(state, room, pearls[i]);
                items::string_pearl sp;
                sp.item = uint16_t(it.index());
                sp.seed = w.item_get_seed(it);
                sp.rad = strung[i].second;
                sp.active = 1;
                w.pearl_string_get_pearls(strung[i].first).push_back(sp);
            }
        }

        // the ScavengerTreasury constructor (on the room's first visit)
        void spawn_treasury(sys::game_state& state, dcon::room_id room, dcon::placed_thing_id t) {
            auto& w = state.world;
            auto const view = world::get_room_view(state, room);
            vec2 const pos = w.placed_thing_get_pos(t);
            float const rad = rw::magnitude(w.placed_thing_get_handle(t));
            ivec2 const tp = room_view::get_tile_position(pos);
            int32_t const n = int32_t(rad / 20.f);
            std::vector<ivec2> tiles;
            for (int32_t i = tp.x - n; i <= tp.x + n; ++i)
                for (int32_t j = tp.y - n; j <= tp.y + n; ++j)
                    if (custom::dist_less(room_view::middle_of_tile(i, j), pos, rad) && !view.get_tile(i, j).solid() && !view.get_tile(i, j + 1).solid()
                        && view.get_tile(i, j - 1).solid())
                        tiles.push_back(ivec2{i, j});
            // the regions with more lanterns, and Downpour's electric spears per timeline
            auto const rid = w.room_get_region_from_region_membership(room);
            std::string_view const region = rid ? state.to_string_view(w.region_get_name(rid)) : std::string_view{};
            bool const lantern_region = region == "SH" || region == "SB";
            float elec = 0.f, charged = 0.f;
            switch (state.story_slugcat) {
            case ::player::slugcat_name::artificer:
            case ::player::slugcat_name::spear:
                elec = 0.5f;
                charged = 0.75f;
                break;
            case ::player::slugcat_name::red:
            case ::player::slugcat_name::gourmand:
                elec = 0.05f;
                charged = 0.25f;
                break;
            case ::player::slugcat_name::white:
            case ::player::slugcat_name::yellow:
                elec = 0.02f;
                charged = 0.01f;
                break;
            default:
                break;
            }
            if (region == "LC") {
                elec = 0.75f;
                charged = 1.f;
            }
            std::vector<pending> loot;
            auto& rng = state.room_random(room);
            for (auto const tile : tiles) {
                vec2 const at = room_view::middle_of_tile(tile);
                if (!(rng.value() < mathf::inverse_lerp(rad, rad / 5.f, rw::distance(at, pos))))
                    continue;
                if (rng.value() < 0.1f) {
                    loot.push_back(pending{world::item_type::data_pearl, at, -1, true});
                } else if (rng.value() < 0.14285715f) {
                    loot.push_back(pending{world::item_type::bomb, at});
                } else if (rng.value() < 1.f / (lantern_region ? 5.f : 20.f)) {
                    // a Lantern: not ported, the spot stays empty
                } else {
                    pending p{rng.value() < 0.75f ? world::item_type::explosive_spear : world::item_type::spear, at};
                    // ModManager.DLCShared
                    if (rng.value() < elec) {
                        p.type = world::item_type::electric_spear;
                        if (rng.value() >= charged)
                            p.charge = 0;
                    }
                    loot.push_back(p);
                }
            }
            auto const tr = w.create_treasury();
            w.treasury_set_pos(tr, pos);
            w.treasury_set_rad(tr, rad);
            w.treasury_set_loot(tr, uint16_t(loot.size()));
            w.try_create_treasury_location(tr, room);
            for (auto const& p : loot)
                realize(state, room, p);
        }

        // Room.SpawnMultiplayerItem (story sessions)
        void spawn_multiplayer_item(sys::game_state& state, dcon::room_id room, dcon::placed_thing_id t) {
            auto& w = state.world;
            if (state.room_random(room).value() > w.placed_thing_get_extra(t))
                return;
            std::string_view const kind = state.to_string_view(w.placed_thing_get_name(t));
            world::item_type type = world::item_type::rock;
            if (kind == "spear")
                type = world::item_type::spear;
            else if (kind == "explosivespear")
                type = world::item_type::explosive_spear;
            else if (kind == "bomb")
                type = world::item_type::bomb;
            else if (kind == "sporeplant")
                return; // not ported
            items::create_on_tile(state, room, w.placed_thing_get_pos(t), type);
        }
    }

    void spawn_room(sys::game_state& state, dcon::room_id room) {
        auto& w = state.world;
        w.room_for_each_thing_location(room, [&](dcon::thing_location_id l) {
            auto const t = w.thing_location_get_thing(l);
            switch (w.placed_thing_get_kind(t)) {
            case world::thing_kind::scav_outpost:
                spawn_outpost(state, room, t);
                break;
            case world::thing_kind::scav_treasury:
                spawn_treasury(state, room, t);
                break;
            case world::thing_kind::multiplayer_item:
                spawn_multiplayer_item(state, room, t);
                break;
            default:
                break;
            }
        });
    }

    int32_t generate_antlers(rw::unity_random& r, float rad, std::vector<items::antler_point>& out) {
        auto const parts = generate_antlers(r, rad);
        out.clear();
        for (size_t i = 0; i < parts.size(); ++i)
            for (size_t k = 0; k < parts[i].positions.size(); ++k) {
                items::antler_point ap;
                ap.pos = parts[i].positions[k];
                ap.rad = parts[i].rads[k];
                ap.ind[0] = parts[i].ind[0][k];
                ap.ind[1] = parts[i].ind[1][k];
                ap.part = uint8_t(i);
                out.push_back(ap);
            }
        return int32_t(parts.size());
    }

    vec3 transformed_pos(items::antler_point const& p, float flip) noexcept {
        return p.pos + p.ind[flip < 0.f ? 0 : 1];
    }

    vec2 transform_to_head_rotat(vec3 dp, vec2 antler_pos, float rotation, float flip, float face_dir) noexcept {
        float const deg = mathf::lerp(-116.99999f, 116.99999f, mathf::inverse_lerp(-1.3f, 1.3f, face_dir));
        dp.x *= flip;
        vec2 const turned = body_parts::rotate_around_origo(vec2{dp.x, dp.z}, deg);
        return body_parts::rotate_around_origo(vec2{turned.x, dp.y}, rotation) + antler_pos;
    }

    vec2 attached_pos(sys::game_state const& state, dcon::pearl_string_id s) {
        auto const& w = state.world;
        auto const o = w.pearl_string_get_outpost_from_string_outpost(s);
        auto const points = w.outpost_get_antlers(o);
        uint8_t const part = w.pearl_string_get_on_part(s);
        float const side = float(w.pearl_string_get_on_side(s));
        uint32_t k = 0, found = 0;
        for (; k < points.size(); ++k)
            if (points.at(k).part == part) {
                if (found == w.pearl_string_get_on_pos(s))
                    break;
                ++found;
            }
        if (k >= points.size())
            return w.outpost_get_antler_pos(o);
        float const rotation = custom::vec_to_deg(rw::normalized(w.outpost_get_handle(o)));
        return transform_to_head_rotat(transformed_pos(points.at(k), side), w.outpost_get_antler_pos(o), rotation, side, w.outpost_get_antler_flip(o));
    }

    dcon::item_id pearl_item(sys::game_state const& state, items::string_pearl const& p) noexcept {
        if (p.item == 0xFFFF)
            return dcon::item_id{};
        dcon::item_id const it{dcon::item_id::value_base_t(p.item)};
        auto const& w = state.world;
        if (!w.item_is_valid(it) || w.item_get_seed(it) != p.seed || w.item_get_type(it) != world::item_type::data_pearl)
            return dcon::item_id{};
        return it;
    }

    void update_room(sys::game_state& state, room_view const& room) {
        auto& w = state.world;
        w.room_for_each_outpost_location(room.id, [&](dcon::outpost_location_id ol) {
            auto const o = w.outpost_location_get_outpost(ol);
            w.outpost_for_each_string_outpost(o, [&](dcon::string_outpost_id so) {
                auto const s = w.string_outpost_get_string(so);
                auto pearls = w.pearl_string_get_pearls(s);
                uint32_t const n = pearls.size();
                if (n == 0)
                    return;
                // a pearl gone or held breaks its connections for good
                for (uint32_t i = 0; i < n; ++i) {
                    auto const it = pearl_item(state, pearls.at(i));
                    if (!it || w.physical_object_get_room_from_object_location(items::body(state, it)) != room.id || items::grabber(state, it))
                        pearls.at(i).active = 0;
                }
                auto const pos = [&](uint32_t i) -> vec2& { return w.body_chunk_get_pos(items::chunk(state, pearl_item(state, pearls.at(i)))); };
                auto const vel = [&](uint32_t i) -> vec2& { return w.body_chunk_get_vel(items::chunk(state, pearl_item(state, pearls.at(i)))); };
                auto const pull = [&](uint32_t a, uint32_t b, float rad) {
                    float const d = rw::distance(pos(a), pos(b));
                    if (d <= rad)
                        return;
                    vec2 const dir = rw::normalized(pos(a) - pos(b));
                    pos(a) += dir * (rad - d) * 0.5f;
                    vel(a) += dir * (rad - d) * 0.5f;
                    pos(b) -= dir * (rad - d) * 0.5f;
                    vel(b) -= dir * (rad - d) * 0.5f;
                };
                // PearlString.Attach: the first pearl at its distance from the antler
                auto const attach = [&] {
                    if (!pearls.at(0).active)
                        return;
                    vec2 const at = attached_pos(state, s);
                    vec2 const dir = rw::normalized(pos(0) - at);
                    float const d = rw::distance(pos(0), at);
                    pos(0) += dir * (pearls.at(0).rad - d);
                    vel(0) += dir * (pearls.at(0).rad - d);
                };
                for (uint32_t j = 1; j < n; ++j)
                    if (pearls.at(j).active && pearls.at(j - 1).active)
                        pull(j, j - 1, pearls.at(j).rad);
                attach();
                for (uint32_t k = n - 1; k-- > 0;)
                    if (pearls.at(k).active && pearls.at(k + 1).active)
                        pull(k, k + 1, pearls.at(k).rad);
                attach();
            });
        });
    }
}
