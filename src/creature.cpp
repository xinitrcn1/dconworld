#include <algorithm>
#include <string>

#include "creature.hpp"
#include "dens.hpp"
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
#include "tube_worm.hpp"
#include "tube_worm_graphics.hpp"
#include "needle_worm.hpp"
#include "needle_worm_graphics.hpp"
#include "pole_mimic.hpp"
#include "pole_mimic_graphics.hpp"
#include "tentacle_plant.hpp"
#include "tentacle_plant_graphics.hpp"
#include "jetfish.hpp"
#include "jetfish_graphics.hpp"
#include "scavenger.hpp"
#include "scavenger_graphics.hpp"
#include "deer.hpp"
#include "deer_graphics.hpp"
#include "miros.hpp"
#include "miros_graphics.hpp"
#include "migrate.hpp"
#include "physics.hpp"
#include "piggyback.hpp"
#include "player.hpp"
#include "items.hpp"
#include "rooms.hpp"
#include "tracker.hpp"

namespace creature {
    using rw::vec2;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    char const* to_string(type t) noexcept {
        switch (t) {
        case type::slugcat: return "slugcat";
        case type::centipede: return "centipede";
        case type::dropbug: return "dropwig";
        case type::cicada: return "squidcada";
        case type::lizard: return "lizard";
        case type::daddy: return "daddy long legs";
        case type::leech: return "leech";
        case type::tube_worm: return "grapple worm";
        case type::needle_worm: return "noodlefly";
        case type::pole_mimic: return "pole plant";
        case type::tentacle_plant: return "monster kelp";
        case type::jetfish: return "jetfish";
        case type::scavenger: return "scavenger";
        case type::deer: return "rain deer";
        case type::miros: return "miros bird";
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
        // out in a room, no den yet (dens.hpp)
        w.creature_set_home_room(c, dcon::room_id{});
        w.creature_set_home_den(c, dens::no_den);
        w.creature_set_away(c, 0);
        w.creature_set_away_room(c, dcon::room_id{});
        w.creature_set_entering_shortcut(c, rw::ivec2{-1, -1});
        w.creature_set_shortcut_rad_add(c, 0.f);
        w.creature_set_shortcut_delay(c, 0);
        w.creature_set_vessel_shortcut(c, dcon::shortcut_id{});
        w.creature_set_vessel_reverse(c, 0);
        w.creature_set_vessel_progress(c, 0.f);
        w.creature_set_remain_in_den(c, -1);
        w.creature_set_going_home(c, 0);
        w.creature_set_travel_tile(c, rw::ivec2{-1, -1});
        w.creature_set_rain_death(c, 0.f);
        w.creature_set_spawn_flags(c, 0);
        w.creature_set_vessel_last_progress(c, 0.f);
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_grasps(c, grasp_pair{});
        w.creature_set_killed_by(c, 0);
        switch (t) {
        case type::slugcat:
            pl.slugcat = slugcat_payload{};
            break;
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
        case type::tube_worm:
            pl.tube_worm = tube_worm_payload{};
            pl.tube_worm.data = uint16_t(w.create_tube_worm_data().index());
            break;
        case type::needle_worm:
            pl.needle_worm = needle_worm_payload{};
            pl.needle_worm.data = uint16_t(w.create_needle_worm_data().index());
            break;
        case type::pole_mimic:
            pl.pole_mimic = pole_mimic_payload{};
            pl.pole_mimic.data = uint16_t(w.create_pole_mimic_data().index());
            break;
        case type::tentacle_plant:
            pl.tentacle_plant = tentacle_plant_payload{};
            pl.tentacle_plant.data = uint16_t(w.create_tentacle_plant_data().index());
            break;
        case type::jetfish:
            pl.jetfish = jetfish_payload{};
            pl.jetfish.data = uint16_t(w.create_jetfish_data().index());
            break;
        case type::scavenger:
            pl.scavenger = scavenger_payload{};
            pl.scavenger.data = uint16_t(w.create_scavenger_data().index());
            break;
        case type::deer:
            pl.deer = deer_payload{};
            pl.deer.data = uint16_t(w.create_deer_data().index());
            break;
        case type::miros:
            pl.miros = miros_payload{};
            pl.miros.data = uint16_t(w.create_miros_data().index());
            break;
        case type::count:
            break;
        }
        return c;
    }

    void erase(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        switch (w.creature_get_type(c)) {
        case type::slugcat: break;
        case type::centipede: w.delete_centipede_data(centipede_data(w, c)); break;
        case type::dropbug: w.delete_dropbug_data(dropbug_data(w, c)); break;
        case type::cicada: w.delete_cicada_data(cicada_data(w, c)); break;
        case type::lizard: w.delete_lizard_data(lizard_data(w, c)); break;
        case type::daddy: w.delete_daddy_data(daddy_data(w, c)); break;
        case type::leech: w.delete_leech_data(leech_data(w, c)); break;
        case type::tube_worm: w.delete_tube_worm_data(tube_worm_data(w, c)); break;
        case type::needle_worm: w.delete_needle_worm_data(needle_worm_data(w, c)); break;
        case type::pole_mimic: w.delete_pole_mimic_data(pole_mimic_data(w, c)); break;
        case type::tentacle_plant: w.delete_tentacle_plant_data(tentacle_plant_data(w, c)); break;
        case type::jetfish: w.delete_jetfish_data(jetfish_data(w, c)); break;
        case type::scavenger: w.delete_scavenger_data(scavenger_data(w, c)); break;
        case type::deer: w.delete_deer_data(deer_data(w, c)); break;
        case type::miros: w.delete_miros_data(miros_data(w, c)); break;
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
            case part_kind::deer_leg:
                w.delete_deer_leg(dcon::deer_leg_id{dcon::deer_leg_id::value_base_t(pp.deer_leg.data)});
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

    //
    // Other creatures
    //

    dcon::player_id player_of(sys::game_state const& state, dcon::creature_id c) noexcept {
        if (!live(state, c) || !is_slugcat(state.world, c))
            return dcon::player_id{};
        return dcon::player_id{dcon::player_id::value_base_t(state.world.creature_get_payload(c).slugcat.player)};
    }

    dcon::physical_object_id body_of(sys::game_state const& state, dcon::creature_id c) noexcept {
        return live(state, c) ? body(state, c) : dcon::physical_object_id{};
    }

    uint32_t chunk_count_of(sys::game_state const& state, dcon::creature_id c) noexcept {
        return live(state, c) ? chunk_count(state, c) : 0;
    }

    dcon::body_chunk_id chunk_of(sys::game_state const& state, dcon::creature_id c, uint32_t i) noexcept {
        return physics::chunk(state, body_of(state, c), i);
    }

    vec2 main_pos(sys::game_state const& state, dcon::creature_id c) noexcept {
        auto const o = body_of(state, c);
        if (!o)
            return vec2{0.f, 0.f};
        return state.world.body_chunk_get_pos(physics::chunk(state, o, main_chunk(state, c)));
    }

    float mass_of(sys::game_state const& state, dcon::creature_id c) noexcept {
        return live(state, c) ? total_mass(state, c) : 0.f;
    }

    bool eaten(sys::game_state const& state, dcon::creature_id c) noexcept {
        auto const p = player_of(state, c);
        return p && state.world.player_get_eaten(p);
    }

    bool here(sys::game_state const& state, dcon::creature_id c, dcon::room_id room) noexcept {
        auto const& w = state.world;
        if (!live(state, c) || w.creature_get_away(c) != 0 || !body(state, c) || room_of(state, c) != room)
            return false;
        // an eaten slugcat is dead and gone (Destroy in vanilla)
        return !is_slugcat(w, c) || !w.player_get_eaten(player_of(state, c));
    }

    //
    // Grasps
    //

    namespace {
        void hold(sys::game_state& state, dcon::creature_id c, uint32_t i, dcon::physical_object_id o, uint32_t k) {
            auto& w = state.world;
            if (grasped_object(w, c, i))
                release_grasp(state, c, i);
            auto& g = w.creature_get_grasps(c);
            g.obj[i] = o;
            g.chunk[i] = uint8_t(k);
            if (o)
                ++w.physical_object_get_grabbed_by_count(o);
        }
    }

    void grab(sys::game_state& state, dcon::creature_id c, uint32_t i, dcon::creature_id t, uint32_t k) {
        hold(state, c, i, body_of(state, t), k);
    }

    void grab_item(sys::game_state& state, dcon::creature_id c, uint32_t i, dcon::item_id it) {
        auto& w = state.world;
        hold(state, c, i, w.item_get_body_from_item_body(it), 0);
        w.item_set_grabbed_by(it, c);
        w.item_set_grabbed_hand(it, uint8_t(i));
    }

    void release_grasp(sys::game_state& state, dcon::creature_id c, uint32_t i) {
        auto& w = state.world;
        auto& g = w.creature_get_grasps(c);
        auto const o = g.obj[i];
        if (!o)
            return;
        uint8_t& n = w.physical_object_get_grabbed_by_count(o);
        if (n > 0)
            --n;
        if (auto const it = w.physical_object_get_item_from_item_body(o); it && w.item_get_grabbed_by(it) == c)
            w.item_set_grabbed_by(it, dcon::creature_id{});
        g.obj[i] = dcon::physical_object_id{};
        g.chunk[i] = 0;
    }

    dcon::creature_id holder(sys::game_state const& state, dcon::creature_id t, uint32_t* grasp) noexcept {
        dcon::creature_id h;
        if (!live(state, t))
            return h;
        for_each_holder(state, room_of(state, t), t, [&](dcon::creature_id o, uint32_t i) {
            if (h)
                return;
            h = o;
            if (grasp)
                *grasp = i;
        });
        return h;
    }

    //
    // Fights
    //

    kill_counts& kills() noexcept {
        static kill_counts k;
        return k;
    }

    namespace {
        void killed(sys::game_state& state, dcon::creature_id t, tmpl source) {
            state.world.creature_set_killed_by(t, uint8_t(uint32_t(source) + 1));
            ++kills().n[size_t(source)][size_t(template_of(state, t))];
        }
    }

    void violence_on(sys::game_state& state, dcon::creature_id t, uint32_t k, vec2 momentum, float damage, float stun_bonus, damage_type type, tmpl source) {
        if (!live(state, t))
            return;
        bool const was_dead = state.world.creature_get_dead(t) != 0;
        violence(state, t, hit_part{int32_t(k)}, momentum, damage, stun_bonus, type);
        if (!was_dead && state.world.creature_get_dead(t))
            killed(state, t, source);
    }

    void kill(sys::game_state& state, dcon::creature_id t, tmpl source) {
        if (!live(state, t) || state.world.creature_get_dead(t))
            return;
        die(state, t);
        killed(state, t, source);
    }

    //
    // The dispatch
    //

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 pos, spawn_desc const& d) {
        switch (d.kind) {
        case type::slugcat: break;
        case type::centipede: return centipede::create(state, room, pos, d.size, centipede::variant(d.variety % centipede::variant_count));
        case type::dropbug: return dropbug::create(state, room, pos, d.relocate);
        case type::cicada: return cicada::create(state, room, pos, d.size != 0.f);
        case type::lizard: return lizard::create(state, room, pos, lizard::breed(d.variety % lizard::breed_count));
        case type::daddy: return daddy::create(state, room, pos, daddy::variant(d.variety % daddy::variant_count));
        case type::leech: return leech::create(state, room, pos, leech::variant(d.variety % leech::variant_count));
        case type::tube_worm: return tube_worm::create(state, room, pos);
        case type::needle_worm: return needle_worm::create(state, room, pos, needle_worm::variant(d.variety % needle_worm::variant_count));
        case type::pole_mimic: return pole_mimic::create(state, room, pos);
        case type::tentacle_plant: return tentacle_plant::create(state, room, pos);
        case type::jetfish: return jetfish::create(state, room, pos);
        case type::scavenger: return scavenger::create(state, room, pos);
        case type::deer: return deer::create(state, room, pos);
        case type::miros: return miros::create(state, room, pos);
        case type::count: break;
        }
        return dcon::creature_id{};
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        switch (state.world.creature_get_type(c)) {
        case type::slugcat: player::destroy(state, player_of(state, c)); return;
        case type::centipede: centipede::destroy(state, c); return;
        case type::dropbug: dropbug::destroy(state, c); return;
        case type::cicada: cicada::destroy(state, c); return;
        case type::lizard: lizard::destroy(state, c); return;
        case type::daddy: daddy::destroy(state, c); return;
        case type::leech: leech::destroy(state, c); return;
        case type::tube_worm: tube_worm::destroy(state, c); return;
        case type::needle_worm: needle_worm::destroy(state, c); return;
        case type::pole_mimic: pole_mimic::destroy(state, c); return;
        case type::tentacle_plant: tentacle_plant::destroy(state, c); return;
        case type::jetfish: jetfish::destroy(state, c); return;
        case type::scavenger: scavenger::destroy(state, c); return;
        case type::deer: deer::destroy(state, c); return;
        case type::miros: miros::destroy(state, c); return;
        case type::count: return;
        }
    }

    std::string describe(sys::game_state const& state, dcon::creature_id c) {
        auto const& w = state.world;
        if (w.creature_get_away(c) != 0) {
            auto const room = w.creature_get_away_room(c);
            auto const where = std::string{w.creature_get_away(c) == 1 ? "in a pipe" : "in a den"} + " of " + std::string{state.to_string_view(w.room_get_name(room))};
            return std::string{to_string(w.creature_get_type(c))} + " " + where + (w.creature_get_away(c) == 2 ? " (out in " + std::to_string(w.creature_get_remain_in_den(c)) + " ticks)" : "");
        }
        auto const at = [](vec2 p) { return std::to_string(int(p.x)) + "," + std::to_string(int(p.y)); };
        std::string const behavior = std::to_string(int(w.creature_get_behavior(c)));
        std::string const dead = w.creature_get_dead(c) ? " dead" : "";
        auto const& pl = w.creature_get_payload(c);
        switch (w.creature_get_type(c)) {
        case type::slugcat:
            return "slugcat at " + at(w.body_chunk_get_pos(chunk(state, c, 0))) + dead;
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
                + " legs " + std::to_string(w.lizard_data_get_graphics(x).legs_grabbing) + (lizard::grasped(state, c) ? std::string{" holding a "} + to_string(w.creature_get_type(lizard::grasped(state, c))) : std::string{})
                + (w.lizard_data_get_tongue(x).state != lizard::tongue_state::hidden ? " tongue out" : "") + dead;
        }
        case type::daddy: {
            bool held = false, eating = false;
            w.for_each_player([&](dcon::player_id p) {
                held = held || daddy::holds(state, c, w.player_get_creature(p));
                eating = eating || daddy::eating(state, c, w.player_get_creature(p));
            });
            return std::string{daddy::to_string(pl.daddy.variant)} + " long legs at " + at(daddy::middle_of_body(state, c))
                + " behavior " + behavior + " chunks " + std::to_string(chunk_count(state, c))
                + " legs " + std::to_string(daddy::legs_gripping(state, c)) + "/" + std::to_string(pl.daddy.tentacle_count)
                + (held ? " holding the slugcat" : "") + (eating ? " eating the slugcat" : "") + dead;
        }
        case type::leech:
            return std::string{leech::to_string(pl.leech.variant)} + " at " + at(w.body_chunk_get_pos(chunk(state, c, 0))) + " school of "
                + std::to_string(leech::school_size(state, c)) + (leech::attached_to(state, c) ? " latched on" : "")
                + (pl.leech.hunt_prey ? " hunting" : "") + dead;
        case type::tube_worm:
            return "grapple worm at " + at(w.body_chunk_get_pos(chunk(state, c, 0))) + " tongues " + tube_worm::describe_tongues(state, c)
                + (pl.tube_worm.sleeping ? " sleeping" : "") + (held(state, c) ? " held" : "") + dead;
        case type::needle_worm:
            return needle_worm::describe(state, c) + dead;
        case type::pole_mimic:
            return pole_mimic::describe(state, c) + dead;
        case type::tentacle_plant:
            return tentacle_plant::describe(state, c) + dead;
        case type::jetfish:
            return jetfish::describe(state, c) + dead;
        case type::scavenger:
            return scavenger::describe(state, c) + dead;
        case type::deer:
            return deer::describe(state, c) + dead;
        case type::miros:
            return miros::describe(state, c) + dead;
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

        void update_kind(sys::game_state& state, dcon::creature_id c, world::room_view const& room, bool viewed);

        // the kind's Update, and its graphics module's where a camera looks
        void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room, bool viewed) {
            // squeezing into a pipe: only that (Creature.Update's enteringShortCut)
            if (dens::before_update(state, c, room))
                return;
            migrate::tick(state, c);
            update_kind(state, c, room, viewed);
            // close to the pipe home: in it goes
            dens::after_update(state, c, room);
        }

        void update_kind(sys::game_state& state, dcon::creature_id c, world::room_view const& room, bool viewed) {
            switch (state.world.creature_get_type(c)) {
            // (player.cpp runs the slugcats, before the room's creatures)
            case type::slugcat:
                return;
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
            case type::tube_worm:
                tube_worm::update(state, c, room);
                if (viewed)
                    tube_worm_graphics::update(state, c, room);
                return;
            case type::needle_worm:
                needle_worm::update(state, c, room);
                if (viewed)
                    needle_worm_graphics::update(state, c, room);
                return;
            case type::pole_mimic:
                pole_mimic::update(state, c, room);
                if (viewed)
                    pole_mimic_graphics::update(state, c, room);
                return;
            case type::tentacle_plant:
                tentacle_plant::update(state, c, room);
                if (viewed)
                    tentacle_plant_graphics::update(state, c, room);
                return;
            case type::jetfish:
                jetfish::update(state, c, room);
                if (viewed)
                    jetfish_graphics::update(state, c, room);
                return;
            case type::scavenger:
                scavenger::update(state, c, room);
                if (viewed)
                    scavenger_graphics::update(state, c, room);
                return;
            case type::deer:
                deer::update(state, c, room);
                if (viewed)
                    deer_graphics::update(state, c, room);
                return;
            case type::miros:
                miros::update(state, c, room);
                if (viewed)
                    miros_graphics::update(state, c, room);
                return;
            case type::count:
                return;
            }
        }

        // `c` collides with `t` at all (what holds it, what it carries, a
        // slugcat in a pipe or being eaten don't)
        bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t, dcon::room_id room) {
            if (!here(state, t, room))
                return false;
            switch (state.world.creature_get_type(c)) {
            // a slugcat is pushed about by what it touches, it doesn't push
            // back on its own (its body's collisions are the creatures')
            case type::slugcat: return true;
            case type::centipede: return centipede::collides(state, c, t);
            case type::dropbug: return dropbug::collides(state, c, t);
            case type::cicada: return cicada::collides(state, c, t);
            case type::lizard: return lizard::collides(state, c, t, room);
            case type::daddy: return daddy::collides(state, c, t, room);
            // a leech is in another collision layer than the rest
            case type::leech: return false;
            case type::tube_worm: return tube_worm::collides(state, c, t);
            case type::needle_worm: return needle_worm::collides(state, c, t);
            case type::pole_mimic: return pole_mimic::collides(state, c, t);
            case type::tentacle_plant: return tentacle_plant::collides(state, c, t);
            case type::jetfish: return jetfish::collides(state, c, t);
            case type::scavenger: return scavenger::collides(state, c, t);
            case type::deer: return deer::collides(state, c, t);
            case type::miros: return miros::collides(state, c, t);
            case type::count: break;
            }
            return false;
        }

        // the kind's Collide: its chunk `i` touched `t`'s chunk `j`
        void collide_with(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j) {
            switch (state.world.creature_get_type(c)) {
            case type::slugcat: return;
            case type::centipede: centipede::collide(state, c, room, i, t, j); return;
            case type::dropbug: dropbug::collide(state, c, room, i, t, j); return;
            case type::cicada: cicada::collide(state, c, room, i, t, j); return;
            case type::lizard: lizard::collide(state, c, room, i, t, j); return;
            case type::daddy: daddy::collide(state, c, i, t, j); return;
            case type::leech: return;
            case type::tube_worm: tube_worm::collide(state, c); return;
            case type::needle_worm: needle_worm::collide(state, c, room, i, t, j); return;
            case type::pole_mimic: pole_mimic::collide(state, c, room, i, t, j); return;
            case type::tentacle_plant: tentacle_plant::collide(state, c, room, i, t, j); return;
            case type::jetfish: jetfish::collide(state, c, room, i, t, j); return;
            case type::scavenger: scavenger::collide(state, c, room, i, t, j); return;
            case type::deer: deer::collide(state, c, room, i, t, j); return;
            case type::miros: miros::collide(state, c, room, i, t, j); return;
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
            case type::tube_worm:
                tube_worm::collide(state, c);
                tube_worm::collide(state, other);
                return;
            default: return;
            }
        }
        // ...and once per pair that touched at all
        void touched_kin(sys::game_state& state, dcon::creature_id c, dcon::creature_id other) {
            if (state.world.creature_get_type(c) == type::dropbug)
                dropbug::touched_kin(state, c, other);
        }

        // a creature's reach from its chunk 0: whether two can touch at all
        float reach(sys::game_state const& state, dcon::creature_id c, uint32_t n) {
            auto const& w = state.world;
            vec2 const o = w.body_chunk_get_pos(chunk(state, c, 0));
            float r = 0.f;
            for (uint32_t i = 0; i < n; ++i) {
                auto const k = chunk(state, c, i);
                r = std::max(r, rw::distance(o, w.body_chunk_get_pos(k)) + w.body_chunk_get_rad(k));
            }
            return r;
        }

        // Room object collisions (Room.Update's PhysicalObject pairs in
        // collision layer 1) of a creature with the slugcats and with the
        // creatures after it (each pair once): pushing apart, both kinds'
        // Collide; its own kind's collide_kin. The slugcats (first in the
        // room's creatures) don't run this: they meet the others in theirs
        void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room) {
            auto const& w = state.world;
            uint32_t const n = chunk_count(state, c);
            auto const t = w.creature_get_type(c);
            if (t == type::leech || t == type::slugcat)
                return;
            float const rc = reach(state, c, n);
            vec2 const oc = w.body_chunk_get_pos(chunk(state, c, 0));
            rooms::for_each_creature(state, room.id, [&](dcon::creature_id other) {
                auto const ot = w.creature_get_type(other);
                if (ot == type::slugcat) {
                    // the slugcat's side has no Collide of its own
                    if (!collides(state, c, other, room.id))
                        return;
                    uint32_t const m = chunk_count(state, other);
                    for (uint32_t i = 0; i < n; ++i)
                        for (uint32_t j = 0; j < m; ++j)
                            if (push_apart(state, chunk(state, c, i), chunk(state, other, j)))
                                collide_with(state, c, room, i, other, j);
                    return;
                }
                if (other.index() <= c.index())
                    return;
                if (ot == type::leech || (ot == t && !pushes_kin(t)) || w.creature_get_away(other) != 0)
                    return;
                uint32_t const m = chunk_count(state, other);
                if (!custom::dist_less(oc, w.body_chunk_get_pos(chunk(state, other, 0)), rc + reach(state, other, m)))
                    return;
                if (ot != t && (!collides(state, c, other, room.id) || !collides(state, other, c, room.id)))
                    return;
                bool touched = false;
                for (uint32_t i = 0; i < n; ++i)
                    for (uint32_t j = 0; j < m; ++j)
                        if (push_apart(state, chunk(state, c, i), chunk(state, other, j))) {
                            touched = true;
                            if (ot == t) {
                                collide_kin(state, c, i, other, j);
                            } else {
                                collide_with(state, c, room, i, other, j);
                                collide_with(state, other, room, j, c, i);
                            }
                        }
                if (touched && ot == t)
                    touched_kin(state, c, other);
            });
        }
    }

    namespace {
        // Slugcats bump into each other too (players are in collision layer
        // 1 like the rest): each pair once, both pushed apart. Not one in a
        // pipe, being eaten or held
        void collide_slugcats(sys::game_state& state, world::room_view const& room) {
            auto const& w = state.world;
            std::vector<dcon::creature_id> cats;
            rooms::for_each_creature(state, room.id, type::slugcat, [&](dcon::creature_id c) {
                // (not one on another's back: SlugOnBack.ChangeOverlap)
                if (w.creature_get_away(c) == 0 && collides(state, c, c, room.id) && !held(state, c) && !piggyback::riding(state, player_of(state, c)))
                    cats.push_back(c);
            });
            for (size_t a = 0; a < cats.size(); ++a)
                for (size_t b = a + 1; b < cats.size(); ++b) {
                    if (!custom::dist_less(w.body_chunk_get_pos(chunk(state, cats[a], 0)), w.body_chunk_get_pos(chunk(state, cats[b], 0)), 80.f))
                        continue;
                    for (uint32_t i = 0; i < 2; ++i)
                        for (uint32_t j = 0; j < 2; ++j)
                            push_apart(state, chunk(state, cats[a], i), chunk(state, cats[b], j));
                }
        }
    }

    void update_room(sys::game_state& state, world::room_view const& room) {
        bool const viewed = state.viewed(room.id);
        collide_slugcats(state, room);
        for (auto const t : all_types) {
            // (the slugcats ran first, player.cpp, and collide in the others' turns)
            if (t == type::slugcat)
                continue;
            rooms::for_each_creature(state, room.id, t, [&](dcon::creature_id c) { update(state, c, room, viewed); });
            // the room's LizardSpit flies after the lizards
            if (t == type::lizard)
                lizard::update_spit(state, room);
            rooms::for_each_creature(state, room.id, t, [&](dcon::creature_id c) {
                if (state.world.creature_is_valid(c) && state.world.creature_get_away(c) == 0)
                    collide(state, c, room);
            });
        }
    }

    void reset_graphics(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        switch (w.creature_get_type(c)) {
        // (PlayerGraphics runs every tick: the slugcat's room is always viewed)
        case type::slugcat: return;
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
        case type::tube_worm: tube_worm_graphics::reset_positions(state, c); return;
        case type::needle_worm: needle_worm_graphics::reset_positions(state, c); return;
        case type::pole_mimic: pole_mimic_graphics::reset_positions(state, c); return;
        case type::tentacle_plant: tentacle_plant_graphics::reset_positions(state, c); return;
        case type::jetfish: jetfish_graphics::reset_positions(state, c); return;
        case type::scavenger: scavenger_graphics::reset_positions(state, c); return;
        case type::deer: deer_graphics::reset_positions(state, c); return;
        case type::miros: miros_graphics::reset_positions(state, c); return;
        case type::count: return;
        }
    }

    void came_into_view(sys::game_state& state, dcon::room_id room) {
        rooms::for_each_creature(state, room, [&](dcon::creature_id c) { reset_graphics(state, c); });
    }

    void placed_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, vec2 at) {
        // what keeps tiles of the room it was in (a daddy's tentacles)
        auto& w = state.world;
        if (w.creature_get_type(c) == type::daddy)
            daddy::place_in_room(state, c, room, at);
        // TubeWorm.SpitOutOfShortCut: the tongues back in
        if (w.creature_get_type(c) == type::tube_worm)
            tube_worm::place_in_room(state, c, at);
        switch (w.creature_get_type(c)) {
        case type::needle_worm: needle_worm::place_in_room(state, c, room, at); break;
        case type::pole_mimic: pole_mimic::place_in_room(state, c, room, at); break;
        case type::tentacle_plant: tentacle_plant::place_in_room(state, c, room, at); break;
        case type::jetfish: jetfish::place_in_room(state, c, room, at); break;
        case type::scavenger: scavenger::place_in_room(state, c, room, at); break;
        case type::deer: deer::place_in_room(state, c, room, at); break;
        case type::miros: miros::place_in_room(state, c, room, at); break;
        default: break;
        }
        // a leech out of the den is done fleeing the rain
        if (w.creature_get_type(c) == type::leech) {
            w.creature_get_payload(c).leech.flee_from_rain = 0;
            w.creature_get_payload(c).leech.has_goal = 0;
        }
        // ArtificialIntelligence.NewRoom: what it tracked is in another room
        tracker::clear(state, c);
        migrate::arrived(state, c, room);
        reset_graphics(state, c);
    }

    void stun(sys::game_state& state, dcon::creature_id c, int32_t st) {
        if (is_slugcat(state.world, c)) {
            player::stun(state, player_of(state, c), st);
            return;
        }
        int32_t& s = state.world.creature_get_stun(c);
        s = std::max(s, st);
    }

    void die(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        if (w.creature_get_dead(c))
            return;
        // (Player.Die: a dead slugcat keeps what it holds)
        if (is_slugcat(w, c)) {
            player::die(state, player_of(state, c));
            return;
        }
        w.creature_set_dead(c, 1);
        release_all(state, c, room_of(state, c));
        // TubeWorm.Die: the slugcat holding it lets go
        if (w.creature_get_type(c) == type::tube_worm)
            tube_worm::died(state, c);
        switch (w.creature_get_type(c)) {
        case type::needle_worm: needle_worm::died(state, c); break;
        case type::pole_mimic: pole_mimic::died(state, c); break;
        case type::tentacle_plant: tentacle_plant::died(state, c); break;
        case type::jetfish: jetfish::died(state, c); break;
        case type::scavenger: scavenger::died(state, c); break;
        case type::deer: deer::died(state, c); break;
        case type::miros: miros::died(state, c); break;
        default: break;
        }
    }

    void lose_all_grasps(sys::game_state& state, dcon::creature_id c) {
        if (!live(state, c))
            return;
        if (is_slugcat(state.world, c))
            items::release_all(state, player_of(state, c));
        else
            release_all(state, c, room_of(state, c));
    }

    void release_all(sys::game_state& state, dcon::creature_id c, dcon::room_id room) {
        auto const& g = state.world.creature_get_grasps(c);
        for (uint32_t i = 0; i < 2; ++i)
            if (auto const t = grasped(state.world, c, i))
                release(state, c, t);
        rooms::for_each_creature(state, room, [&](dcon::creature_id o) {
            if (o != c)
                release(state, c, o);
        });
        // what the kind's code didn't let go of
        for (uint32_t i = 0; i < 2; ++i)
            if (g.obj[i])
                release_grasp(state, c, i);
    }

    bool trace(sys::game_state const& state, dcon::room_id room, vec2 from, vec2 to, float rad, dcon::creature_id thrown_by, dcon::creature_id& hit, hit_part& part) {
        auto const& w = state.world;
        // Weapon.HitThisObject: not its thrower, and a slugcat's weapon
        // passes through the slugcats only with friendly fire off (story
        // mode's default; here it is on, game_state::friendly_fire)
        bool const by_slugcat = thrown_by && w.creature_is_valid(thrown_by) && is_slugcat(w, thrown_by) && !state.friendly_fire;
        auto const stack = thrown_by && w.creature_is_valid(thrown_by) && is_slugcat(w, thrown_by) ? piggyback::bottom(state, player_of(state, thrown_by))
                                                                                                    : dcon::player_id{};
        // the closest of every creature's chunks along the segment, then
        // the appendages (a daddy's tentacles) when they are closer still
        float best = 2.f;
        vec2 const dv = to - from;
        float const len2 = rw::sqr_magnitude(dv);
        rooms::for_each_creature(state, room, [&](dcon::creature_id c) {
            if (c == thrown_by || (by_slugcat && is_slugcat(w, c)) || w.creature_get_away(c) != 0)
                return;
            // a grapple worm in a hand can't be hit (TubeWorm.canBeHitByWeapons),
            // nor a slugcat on another's back (SlugOnBack.ChangeOverlap)
            if (w.creature_get_type(c) == type::tube_worm && held(state, c))
                return;
            if (is_slugcat(w, c) && piggyback::riding(state, player_of(state, c)))
                return;
            // (nor the one it rides on, or anyone in its stack: Spearmaster
            // throws from a back)
            if (stack && is_slugcat(w, c) && piggyback::bottom(state, player_of(state, c)) == stack)
                return;
            uint32_t const n = chunk_count(state, c);
            for (uint32_t i = 0; i < n; ++i) {
                auto const k = chunk(state, c, i);
                vec2 const p = w.body_chunk_get_pos(k);
                float const r = rad + w.body_chunk_get_rad(k);
                // closest approach of the segment to the chunk
                float const f = len2 > 0.f ? mathf::clamp01(rw::dot(p - from, dv) / len2) : 0.f;
                if (custom::dist_less(from + dv * f, p, r) && f < best) {
                    best = f;
                    hit = c;
                    part = hit_part{int32_t(i)};
                }
            }
        });
        // a monster kelp's tentacle (an appendage: its tip chunk takes the hit)
        {
            dcon::creature_id kh;
            float kf = 2.f;
            if (tentacle_plant::trace_tentacle(state, room, from, to, kh, kf) && kf < best && kh != thrown_by) {
                best = kf;
                hit = kh;
                part = hit_part{0};
            }
        }
        // a pole plant's stalk (PoleMimic's appendage) when closer
        {
            dcon::creature_id ph;
            hit_part pp;
            float pf = 2.f;
            if (pole_mimic::trace_stalk(state, room, from, to, ph, pp, pf) && pf < best && ph != thrown_by) {
                best = pf;
                hit = ph;
                part = pp;
            }
        }
        dcon::creature_id th;
        hit_part tp;
        if (daddy::trace_tentacles(state, room, from, to, th, tp)) {
            // the tentacle's hit point along the segment
            float f = 1.f;
            if (best <= 1.f && tp.tentacle >= 0) {
                auto const t = daddy::tentacle_of(state, th, uint32_t(tp.tentacle));
                auto const seg = w.daddy_tentacle_get_segments(t);
                if (uint32_t(tp.prev_segment) < seg.size()) {
                    vec2 const sp = world::room_view::middle_of_tile(seg[uint32_t(tp.prev_segment)]);
                    f = len2 > 0.f ? mathf::clamp01(rw::dot(sp - from, dv) / len2) : 0.f;
                }
            }
            if (best > 1.f || f < best) {
                hit = th;
                part = tp;
                return true;
            }
        }
        return best <= 1.f;
    }

    void violence(sys::game_state& state, dcon::creature_id c, hit_part const& part, vec2 momentum, float damage, float stun_bonus, damage_type type, bool rock) {
        uint32_t const k = part.chunk >= 0 ? uint32_t(part.chunk) : 0u;
        bool const explosion = type == damage_type::explosion;
        switch (state.world.creature_get_type(c)) {
        case type::slugcat: player::violence(state, player_of(state, c), k, momentum, damage, stun_bonus); return;
        case type::centipede: centipede::violence(state, c, k, momentum, damage, stun_bonus, type); return;
        case type::dropbug: dropbug::violence(state, c, k, momentum, damage, stun_bonus); return;
        case type::cicada: cicada::violence(state, c, k, momentum, damage, stun_bonus); return;
        case type::lizard: lizard::violence(state, c, k, momentum, damage, stun_bonus, rock, explosion); return;
        case type::daddy: daddy::violence(state, c, part, momentum, damage, stun_bonus, explosion); return;
        case type::leech: leech::violence(state, c, momentum, damage, stun_bonus); return;
        case type::tube_worm: tube_worm::violence(state, c, k, momentum, damage, stun_bonus); return;
        case type::needle_worm: needle_worm::violence(state, c, k, momentum, damage, stun_bonus, type); return;
        case type::pole_mimic: pole_mimic::violence(state, c, part, momentum, damage, stun_bonus, type); return;
        case type::tentacle_plant: tentacle_plant::violence(state, c, k, momentum, damage, stun_bonus, type); return;
        case type::jetfish: jetfish::violence(state, c, k, momentum, damage, stun_bonus, type); return;
        case type::scavenger: scavenger::violence(state, c, k, momentum, damage, stun_bonus, type); return;
        case type::deer: deer::violence(state, c, k, momentum, damage, stun_bonus, type); return;
        case type::miros: miros::violence(state, c, k, momentum, damage, stun_bonus, type); return;
        case type::count: return;
        }
    }

    void blast(sys::game_state& state, dcon::room_id room, vec2 at, float rad, float force, float damage, float stun, float kill_rad, dcon::physical_object_id source) {
        auto const& w = state.world;
        rooms::for_each_creature(state, room, [&](dcon::creature_id c) {
            if (is_slugcat(w, c)) {
                // a slugcat close to it dies, further out it is stunned
                if (kill_rad <= 0.f || body(state, c) == source || w.creature_get_away(c) != 0 || w.creature_get_dead(c))
                    return;
                vec2 const cp = w.body_chunk_get_pos(chunk(state, c, 0));
                if (custom::dist_less(cp, at, kill_rad))
                    die(state, c);
                else if (custom::dist_less(cp, at, rad))
                    creature::stun(state, c, int32_t(stun * (1.f - rw::distance(cp, at) / rad)));
                return;
            }
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
            // a daddy long legs is blind; the slugcat only sees white
            if (w.creature_get_type(c) == type::daddy || is_slugcat(w, c))
                return;
            uint32_t const m = main_chunk(state, c);
            vec2 const cp = w.body_chunk_get_pos(chunk(state, c, m));
            if (custom::dist_less(cp, at, rad) && room.visual_contact(at, cp))
                violence(state, c, hit_part{int32_t(m)}, vec2{0.f, 0.f}, 0.f, 120.f);
        });
    }

    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t) {
        switch (state.world.creature_get_type(c)) {
        case type::slugcat: break;
        case type::centipede: centipede::release(state, c, t); break;
        case type::dropbug: dropbug::release(state, c, t); break;
        case type::cicada: cicada::release(state, c, t); break;
        case type::lizard: lizard::release(state, c, t); break;
        case type::daddy: daddy::release(state, c, t); break;
        case type::leech: leech::release(state, c, t); break;
        case type::tube_worm: tube_worm::release(state, c, t); break;
        case type::needle_worm: needle_worm::release(state, c, t); break;
        case type::pole_mimic: pole_mimic::release(state, c, t); break;
        case type::tentacle_plant: tentacle_plant::release(state, c, t); break;
        case type::jetfish: jetfish::release(state, c, t); break;
        case type::scavenger: scavenger::release(state, c, t); break;
        case type::deer: deer::release(state, c, t); break;
        case type::miros: miros::release(state, c, t); break;
        case type::count: break;
        }
        // the generic grasps on it
        auto const& g = state.world.creature_get_grasps(c);
        auto const o = state.world.creature_get_body_from_creature_body(t);
        for (uint32_t i = 0; i < 2; ++i)
            if (o && g.obj[i] == o)
                release_grasp(state, c, i);
    }

    void release_target(sys::game_state& state, dcon::creature_id t, dcon::room_id room) {
        // whatever holds it is in its room
        rooms::for_each_creature(state, room, [&](dcon::creature_id c) {
            if (c != t)
                release(state, c, t);
        });
    }

    void release_target(sys::game_state& state, dcon::creature_id t) {
        auto const& w = state.world;
        release_target(state, t, w.creature_get_away(t) != 0 ? w.creature_get_away_room(t) : room_of(state, t));
    }

    void flying_weapon(sys::game_state& state, dcon::room_id room, vec2 pos, vec2 vel, vec2 thrown_pos) {
        rooms::for_each_creature(state, room, type::dropbug, [&](dcon::creature_id c) { dropbug::flying_weapon(state, c, pos, vel, thrown_pos); });
        rooms::for_each_creature(state, room, type::needle_worm, [&](dcon::creature_id c) { needle_worm::flying_weapon(state, c, pos, vel); });
    }
}
