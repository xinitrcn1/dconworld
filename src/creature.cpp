#include <string>

#include "creature.hpp"
#include "centipede.hpp"
#include "centipede_graphics.hpp"
#include "cicada.hpp"
#include "cicada_graphics.hpp"
#include "daddy.hpp"
#include "daddy_graphics.hpp"
#include "dropbug.hpp"
#include "dropbug_graphics.hpp"
#include "leech.hpp"
#include "leech_graphics.hpp"
#include "lizard.hpp"
#include "lizard_graphics.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "rooms.hpp"

namespace creature {
    using rw::vec2;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    char const* to_string(type t) noexcept {
        switch (t) {
        case type::centipede: return "centipede";
        case type::dropbug: return "dropwig";
        case type::cicada: return "squidcada";
        case type::lizard: return "lizard";
        case type::daddy: return "daddy long legs";
        case type::leech: return "leech";
        case type::count: break;
        }
        return "creature";
    }

    dcon::creature_id make(sys::game_state& state, type t) {
        auto& w = state.world;
        auto const c = w.create_creature();
        w.creature_set_type(c, t);
        auto& pl = w.creature_get_payload(c);
        pl = payload{};
        switch (t) {
        case type::centipede:
            pl.centipede = centipede_payload{};
            pl.centipede.data = uint16_t(w.create_centipede_data().index());
            break;
        case type::dropbug:
            pl.dropbug = dropbug_payload{};
            pl.dropbug.data = uint16_t(w.create_dropbug_data().index());
            break;
        case type::cicada:
            pl.cicada = cicada_payload{};
            pl.cicada.data = uint16_t(w.create_cicada_data().index());
            break;
        case type::lizard:
            pl.lizard = lizard_payload{};
            pl.lizard.data = uint16_t(w.create_lizard_data().index());
            break;
        case type::daddy:
            pl.daddy = daddy_payload{};
            pl.daddy.data = uint16_t(w.create_daddy_data().index());
            break;
        case type::leech:
            pl.leech = leech_payload{};
            pl.leech.data = uint16_t(w.create_leech_data().index());
            break;
        case type::count:
            break;
        }
        return c;
    }

    void erase(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        switch (w.creature_get_type(c)) {
        case type::centipede: w.delete_centipede_data(centipede_data(w, c)); break;
        case type::dropbug: w.delete_dropbug_data(dropbug_data(w, c)); break;
        case type::cicada: w.delete_cicada_data(cicada_data(w, c)); break;
        case type::lizard: w.delete_lizard_data(lizard_data(w, c)); break;
        case type::daddy: w.delete_daddy_data(daddy_data(w, c)); break;
        case type::leech: w.delete_leech_data(leech_data(w, c)); break;
        case type::count: break;
        }
        // its parts and their data objects
        std::vector<dcon::creature_part_id> ps;
        for (auto const o : parts(w, c))
            ps.push_back(w.part_ownership_get_part(o));
        for (auto const p : ps) {
            auto const& pp = w.creature_part_get_payload(p);
            switch (w.creature_part_get_kind(p)) {
            case part_kind::daddy_tentacle:
                w.delete_daddy_tentacle(dcon::daddy_tentacle_id{dcon::daddy_tentacle_id::value_base_t(pp.daddy_tentacle.data)});
                break;
            case part_kind::daddy_tube:
                w.delete_daddy_tube(dcon::daddy_tube_id{dcon::daddy_tube_id::value_base_t(pp.daddy_tube.data)});
                break;
            }
            w.delete_creature_part(p);
        }
        auto const o = body(state, c);
        w.delete_creature(c);
        if (o)
            physics::delete_object(state, o);
    }

    dcon::creature_part_id add_part(sys::game_state& state, dcon::creature_id c, part_kind k, uint8_t index) {
        auto& w = state.world;
        auto const p = w.create_creature_part();
        w.creature_part_set_kind(p, k);
        w.creature_part_set_index(p, index);
        w.creature_part_set_stun(p, 0);
        w.creature_part_set_health(p, 1.f);
        w.creature_part_set_payload(p, part_payload{});
        w.try_create_part_ownership(p, c);
        return p;
    }

    uint32_t chunk_count(sys::game_state const& state, dcon::creature_id c) noexcept {
        auto const r = state.world.physical_object_range_of_chunk_ownership(body(state, c));
        return uint32_t(r.second - r.first);
    }

    dcon::body_chunk_id chunk(sys::game_state const& state, dcon::creature_id c, uint32_t i) noexcept {
        return physics::chunk(state, body(state, c), i);
    }

    dcon::room_id room_of(sys::game_state const& state, dcon::creature_id c) noexcept {
        return state.world.physical_object_get_room_from_object_location(body(state, c));
    }

    float total_mass(sys::game_state const& state, dcon::creature_id c) noexcept {
        float m = 0.f;
        state.world.physical_object_for_each_chunk_ownership_as_owner(body(state, c), [&](dcon::chunk_ownership_id co) {
            m += state.world.body_chunk_get_mass(state.world.chunk_ownership_get_chunk(co));
        });
        return m;
    }

    uint32_t main_chunk(sys::game_state const& state, dcon::creature_id c) noexcept {
        return state.world.creature_get_type(c) == type::centipede ? centipede::head_index(state, c) : 0;
    }

    dcon::player_id player_of(sys::game_state const& state, uint8_t v) noexcept {
        if (v == 0)
            return dcon::player_id{};
        dcon::player_id const p{dcon::player_id::value_base_t(v - 1)};
        return state.world.player_is_valid(p) && rooms::reachable(state, p) ? p : dcon::player_id{};
    }

    //
    // The dispatch
    //

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 pos, spawn_desc const& d) {
        switch (d.kind) {
        case type::centipede: return centipede::create(state, room, pos, d.size, centipede::variant(d.variety % centipede::variant_count));
        case type::dropbug: return dropbug::create(state, room, pos, d.relocate);
        case type::cicada: return cicada::create(state, room, pos, d.size != 0.f);
        case type::lizard: return lizard::create(state, room, pos, lizard::breed(d.variety % lizard::breed_count));
        case type::daddy: return daddy::create(state, room, pos, daddy::variant(d.variety % daddy::variant_count));
        case type::leech: return leech::create(state, room, pos, leech::variant(d.variety % leech::variant_count));
        case type::count: break;
        }
        return dcon::creature_id{};
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        switch (state.world.creature_get_type(c)) {
        case type::centipede: centipede::destroy(state, c); return;
        case type::dropbug: dropbug::destroy(state, c); return;
        case type::cicada: cicada::destroy(state, c); return;
        case type::lizard: lizard::destroy(state, c); return;
        case type::daddy: daddy::destroy(state, c); return;
        case type::leech: leech::destroy(state, c); return;
        case type::count: return;
        }
    }

    std::string describe(sys::game_state const& state, dcon::creature_id c) {
        auto const& w = state.world;
        auto const at = [](vec2 p) { return std::to_string(int(p.x)) + "," + std::to_string(int(p.y)); };
        std::string const behavior = std::to_string(int(w.creature_get_behavior(c)));
        std::string const dead = w.creature_get_dead(c) ? " dead" : "";
        auto const& pl = w.creature_get_payload(c);
        switch (w.creature_get_type(c)) {
        case type::centipede:
            return std::string{centipede::to_string(pl.centipede.variant)} + " at " + at(w.body_chunk_get_pos(chunk(state, c, centipede::head_index(state, c))))
                + " behavior " + behavior + (pl.centipede.flying ? " flying" : "")
                + (pl.centipede.variant == centipede::variant::small ? ""
                    : " shells " + std::to_string(__builtin_popcount(w.centipede_data_get_shells(centipede_data(w, c)))) + "/" + std::to_string(chunk_count(state, c)))
                + dead;
        case type::dropbug:
            return "dropbug at " + at(w.body_chunk_get_pos(chunk(state, c, 0))) + " behavior " + behavior
                + (dropbug::sitting_in_ceiling(state, c) ? " in the ceiling" : "") + dead;
        case type::cicada:
            return std::string{pl.cicada.gender ? "white" : "dark"} + " squidcada at " + at(w.body_chunk_get_pos(chunk(state, c, 0)))
                + " behavior " + behavior + (pl.cicada.flying ? " flying" : " sitting") + " stamina " + std::to_string(pl.cicada.stamina) + dead;
        case type::lizard: {
            auto const x = lizard_data(w, c);
            return std::string{lizard::to_string(pl.lizard.breed)} + " lizard at " + at(w.body_chunk_get_pos(chunk(state, c, 0)))
                + " behavior " + behavior + " animation " + std::to_string(int(w.lizard_data_get_core(x).anim))
                + " legs " + std::to_string(w.lizard_data_get_graphics(x).legs_grabbing) + (lizard::grasped(state, c) ? " holding the slugcat" : "")
                + (w.lizard_data_get_tongue(x).state != lizard::tongue_state::hidden ? " tongue out" : "") + dead;
        }
        case type::daddy: {
            bool held = false, eating = false;
            w.for_each_player([&](dcon::player_id p) {
                held = held || daddy::holds(state, c, p);
                eating = eating || daddy::eating(state, c, p);
            });
            return std::string{daddy::to_string(pl.daddy.variant)} + " long legs at " + at(daddy::middle_of_body(state, c))
                + " behavior " + behavior + " chunks " + std::to_string(chunk_count(state, c))
                + " legs " + std::to_string(daddy::legs_gripping(state, c)) + "/" + std::to_string(pl.daddy.tentacle_count)
                + (held ? " holding the slugcat" : "") + (eating ? " eating the slugcat" : "") + dead;
        }
        case type::leech:
            return std::string{leech::to_string(pl.leech.variant)} + " at " + at(w.body_chunk_get_pos(chunk(state, c, 0))) + " school of "
                + std::to_string(leech::school_size(state, c)) + (leech::attached_to(state, c) ? " latched on to the slugcat" : "")
                + (pl.leech.hunt_prey ? " hunting" : "") + dead;
        case type::count:
            break;
        }
        return "creature";
    }

    namespace {
        // Room object collision response between two chunks, weighted by mass
        bool push_apart(sys::game_state& state, dcon::body_chunk_id a, dcon::body_chunk_id b) {
            auto& w = state.world;
            vec2& pa = w.body_chunk_get_pos(a);
            vec2& pb = w.body_chunk_get_pos(b);
            float const r = w.body_chunk_get_rad(a) + w.body_chunk_get_rad(b);
            if (!custom::dist_less(pa, pb, r))
                return false;
            float const dist = rw::distance(pa, pb);
            vec2 const dir = custom::dir_vec(pa, pb);
            float const ratio = w.body_chunk_get_mass(b) / (w.body_chunk_get_mass(a) + w.body_chunk_get_mass(b));
            pa -= dir * ((r - dist) * ratio);
            w.body_chunk_get_vel(a) -= dir * ((r - dist) * ratio);
            pb += dir * ((r - dist) * (1.f - ratio));
            w.body_chunk_get_vel(b) += dir * ((r - dist) * (1.f - ratio));
            return true;
        }

        // the kind's Update, and its graphics module's where a camera looks
        void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room, bool viewed) {
            switch (state.world.creature_get_type(c)) {
            case type::centipede:
                centipede::update(state, c, room);
                if (viewed)
                    centipede_graphics::update(state, c, room);
                return;
            case type::dropbug:
                dropbug::update(state, c, room);
                if (viewed)
                    dropbug_graphics::update(state, c, room);
                return;
            case type::cicada:
                cicada::update(state, c, room);
                if (viewed)
                    cicada_graphics::update(state, c, room);
                return;
            case type::lizard:
                lizard::update(state, c, room);
                // (the legs' grip the body runs on falls back to
                // Lizard.LegsGripping's 2 without it)
                if (viewed)
                    lizard_graphics::update(state, c, room);
                return;
            case type::daddy:
                daddy::update(state, c, room);
                if (viewed)
                    daddy_graphics::update(state, c, room);
                return;
            case type::leech:
                leech::update(state, c, room);
                if (viewed)
                    leech_graphics::update(state, c, room);
                return;
            case type::count:
                return;
            }
        }

        // the slugcat `p` collides with `c` at all (what holds it, what it
        // carries, a slugcat in a pipe or being eaten don't)
        bool collides(sys::game_state const& state, dcon::creature_id c, dcon::player_id p, dcon::room_id room) {
            switch (state.world.creature_get_type(c)) {
            case type::centipede: return centipede::collides(state, c, p);
            case type::dropbug: return dropbug::collides(state, c, p);
            case type::cicada: return cicada::collides(state, c, p, room);
            case type::lizard: return lizard::collides(state, c, p, room);
            case type::daddy: return daddy::collides(state, c, p, room);
            // a leech is in another collision layer than the slugcat
            case type::leech: return false;
            case type::count: break;
            }
            return false;
        }

        // the kind's Collide with a slugcat whose chunk `j` its chunk `i` touched
        void collide_player(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::player_id p, uint32_t j) {
            switch (state.world.creature_get_type(c)) {
            case type::centipede: centipede::collide_player(state, c, i, p, j); return;
            case type::dropbug: dropbug::collide_player(state, c, i, p, j); return;
            case type::cicada: cicada::collide_player(state, c, i, p, j); return;
            case type::lizard: lizard::collide_player(state, c, room, i, p, j); return;
            case type::daddy: daddy::collide_player(state, c, i, p, j); return;
            case type::leech: return;
            case type::count: return;
            }
        }

        // creatures of a kind push each other apart (daddies don't), and
        // react to it: their chunk `i` and the other's `j` touched
        bool pushes_kin(type t) noexcept { return t != type::daddy; }
        void collide_kin(sys::game_state& state, dcon::creature_id c, uint32_t i, dcon::creature_id other, uint32_t j) {
            switch (state.world.creature_get_type(c)) {
            case type::centipede: centipede::collide_kin(state, c, i, other, j); return;
            case type::dropbug: dropbug::collide_kin(state, c, i, other, j); return;
            case type::cicada: cicada::collide_kin(state, c, i, other, j); return;
            default: return;
            }
        }
        // ...and once per pair that touched at all
        void touched_kin(sys::game_state& state, dcon::creature_id c, dcon::creature_id other) {
            if (state.world.creature_get_type(c) == type::dropbug)
                dropbug::touched_kin(state, c, other);
        }

        // Room object collisions of a creature with the slugcats (pushing
        // apart, the kind's Collide) and with its kin
        void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room) {
            uint32_t const n = chunk_count(state, c);
            rooms::for_each_player(state, room.id, [&](dcon::player_id p) {
                if (!collides(state, c, p, room.id))
                    return;
                for (uint32_t i = 0; i < n; ++i)
                    for (uint32_t j = 0; j < 2; ++j)
                        if (push_apart(state, chunk(state, c, i), player::body_chunk(state, p, j)))
                            collide_player(state, c, room, i, p, j);
            });
            auto const t = state.world.creature_get_type(c);
            if (!pushes_kin(t))
                return;
            rooms::for_each_creature(state, room.id, t, [&](dcon::creature_id other) {
                if (other.index() <= c.index())
                    return;
                uint32_t const m = chunk_count(state, other);
                bool touched = false;
                for (uint32_t i = 0; i < n; ++i)
                    for (uint32_t j = 0; j < m; ++j)
                        if (push_apart(state, chunk(state, c, i), chunk(state, other, j))) {
                            touched = true;
                            collide_kin(state, c, i, other, j);
                        }
                if (touched)
                    touched_kin(state, c, other);
            });
        }
    }

    void update_room(sys::game_state& state, world::room_view const& room) {
        bool const viewed = state.viewed(room.id);
        for (auto const t : all_types) {
            rooms::for_each_creature(state, room.id, t, [&](dcon::creature_id c) { update(state, c, room, viewed); });
            // the room's LizardSpit flies after the lizards
            if (t == type::lizard)
                lizard::update_spit(state, room);
            rooms::for_each_creature(state, room.id, t, [&](dcon::creature_id c) { collide(state, c, room); });
        }
    }

    void came_into_view(sys::game_state& state, dcon::room_id room) {
        auto& w = state.world;
        rooms::for_each_creature(state, room, [&](dcon::creature_id c) {
            switch (w.creature_get_type(c)) {
            case type::centipede: centipede_graphics::reset_positions(state, c); return;
            case type::dropbug: dropbug_graphics::reset_positions(state, c); return;
            case type::cicada: cicada_graphics::reset_positions(state, c); return;
            case type::lizard: {
                lizard_graphics::reset_positions(state, c);
                // no limb has found a grip yet: Lizard.LegsGripping's 2 until
                // the first update
                auto& g = w.lizard_data_get_graphics(lizard_data(w, c));
                g.legs_grabbing = 2;
                g.no_grip_counter = 0;
                return;
            }
            case type::daddy: daddy_graphics::reset(state, c); return;
            case type::leech: leech_graphics::reset_positions(state, c); return;
            case type::count: return;
            }
        });
    }

    bool trace(sys::game_state const& state, dcon::room_id room, vec2 from, vec2 to, float rad, dcon::creature_id& hit, hit_part& part) {
        auto const& w = state.world;
        for (auto const t : all_types) {
            float best = 2.f;
            rooms::for_each_creature(state, room, t, [&](dcon::creature_id c) {
                uint32_t const n = chunk_count(state, c);
                for (uint32_t i = 0; i < n; ++i) {
                    auto const k = chunk(state, c, i);
                    vec2 const p = w.body_chunk_get_pos(k);
                    float const r = rad + w.body_chunk_get_rad(k);
                    // closest approach of the segment to the chunk
                    vec2 const dv = to - from;
                    float const len2 = rw::sqr_magnitude(dv);
                    float const f = len2 > 0.f ? mathf::clamp01(rw::dot(p - from, dv) / len2) : 0.f;
                    if (custom::dist_less(from + dv * f, p, r) && f < best) {
                        best = f;
                        hit = c;
                        part = hit_part{int32_t(i)};
                    }
                }
            });
            if (best <= 1.f)
                return true;
            // the appendages, when no body was hit
            if (t == type::daddy && daddy::trace_tentacles(state, room, from, to, hit, part))
                return true;
        }
        return false;
    }

    void violence(sys::game_state& state, dcon::creature_id c, hit_part const& part, vec2 momentum, float damage, float stun_bonus, damage_type type, bool rock) {
        uint32_t const k = part.chunk >= 0 ? uint32_t(part.chunk) : 0u;
        bool const explosion = type == damage_type::explosion;
        switch (state.world.creature_get_type(c)) {
        case type::centipede: centipede::violence(state, c, k, momentum, damage, stun_bonus, type); return;
        case type::dropbug: dropbug::violence(state, c, k, momentum, damage, stun_bonus); return;
        case type::cicada: cicada::violence(state, c, k, momentum, damage, stun_bonus); return;
        case type::lizard: lizard::violence(state, c, k, momentum, damage, stun_bonus, rock, explosion); return;
        case type::daddy: daddy::violence(state, c, part, momentum, damage, stun_bonus, explosion); return;
        case type::leech: leech::violence(state, c, momentum, damage, stun_bonus); return;
        case type::count: return;
        }
    }

    void blast(sys::game_state& state, dcon::room_id room, vec2 at, float rad, float force, float damage, float stun) {
        auto const& w = state.world;
        rooms::for_each_creature(state, room, [&](dcon::creature_id c) {
            uint32_t const n = chunk_count(state, c);
            for (uint32_t i = 0; i < n; ++i) {
                vec2 const cp = w.body_chunk_get_pos(chunk(state, c, i));
                float const dist = rw::distance(cp, at);
                if (dist < rad) {
                    float const k = 1.f - dist / rad;
                    violence(state, c, hit_part{int32_t(i)}, custom::dir_vec(at, cp) * force * k, damage * k, stun * k, damage_type::explosion);
                    break;
                }
            }
        });
    }

    void flash(sys::game_state& state, world::room_view const& room, vec2 at, float rad) {
        auto const& w = state.world;
        rooms::for_each_creature(state, room.id, [&](dcon::creature_id c) {
            // a daddy long legs is blind
            if (w.creature_get_type(c) == type::daddy)
                return;
            uint32_t const m = main_chunk(state, c);
            vec2 const cp = w.body_chunk_get_pos(chunk(state, c, m));
            if (custom::dist_less(cp, at, rad) && room.visual_contact(at, cp))
                violence(state, c, hit_part{int32_t(m)}, vec2{0.f, 0.f}, 0.f, 120.f);
        });
    }

    void release_player(sys::game_state& state, dcon::creature_id c, dcon::player_id p) {
        switch (state.world.creature_get_type(c)) {
        case type::centipede: centipede::release_player(state, c, p); return;
        case type::dropbug: dropbug::release_player(state, c, p); return;
        case type::cicada: cicada::release_player(state, c, p); return;
        case type::lizard: lizard::release_player(state, c, p); return;
        case type::daddy: daddy::release_player(state, c, p); return;
        case type::leech: leech::release_player(state, c, p); return;
        case type::count: return;
        }
    }

    void release_player(sys::game_state& state, dcon::player_id p) {
        // whatever holds the slugcat is in its room
        rooms::for_each_creature(state, rooms::room_of(state, p), [&](dcon::creature_id c) { release_player(state, c, p); });
    }

    void flying_weapon(sys::game_state& state, dcon::room_id room, vec2 pos, vec2 vel, vec2 thrown_pos) {
        rooms::for_each_creature(state, room, type::dropbug, [&](dcon::creature_id c) { dropbug::flying_weapon(state, c, pos, vel, thrown_pos); });
    }
}
