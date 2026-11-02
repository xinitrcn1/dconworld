#include <algorithm>
#include <cmath>
#include <span>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>
#include <chrono>
#include <memory>
#include <thread>
#include <tuple>

#include <tbb/global_control.h>

#include "support.hpp"
#include "keyed_random.hpp"
#include "tracker.hpp"
#include "gates.hpp"
#include "audio.hpp"
#include "physics.hpp"
#include "centipede.hpp"
#include "cicada.hpp"
#include "dropbug.hpp"
#include "leech.hpp"
#include "tube_worm.hpp"
#include "needle_worm.hpp"
#include "pole_mimic.hpp"
#include "tentacle_plant.hpp"
#include "jetfish.hpp"
#include "scavenger.hpp"
#include "deer.hpp"
#include "miros.hpp"
#include "barnacle.hpp"
#include "egg_bug.hpp"
#include "garbage_worm.hpp"
#include "lantern_mouse.hpp"
#include "loach.hpp"
#include "moth_grub.hpp"
#include "angler.hpp"
#include "batfly.hpp"
#include "big_spider.hpp"
#include "drill_crab.hpp"
#include "frog.hpp"
#include "moth.hpp"
#include "inspector.hpp"
#include "big_eel.hpp"
#include "spider.hpp"
#include "stowaway.hpp"
#include "overseer.hpp"
#include "snail.hpp"
#include "sky_whale.hpp"
#include "yeek.hpp"
#include "vulture.hpp"
#include "lizard.hpp"
#include "daddy.hpp"
#include "items.hpp"
#include "outposts.hpp"
#include "player_graphics.hpp"
#include "pathing.hpp"
#include "player.hpp"
#include "rot.hpp"
#include "warps.hpp"
#include "particles.hpp"
#include "water.hpp"
#include "terrain.hpp"
#include "simulation.hpp"
#include "dens.hpp"
#include "migrate.hpp"
#include "npc.hpp"
#include "piggyback.hpp"
#include "rain.hpp"
#include "spawns.hpp"
#include "world.hpp"

namespace selftest {
    using rw::ivec2;
    using rw::vec2;
    using world::terrain;
    using world::find_spawn;

    dcon::creature_id& world_creatures::of(creature::type t) noexcept {
        switch (t) {
        case creature::type::slugcat: break;
        case creature::type::centipede: return centipede;
        case creature::type::dropbug: return dropbug;
        case creature::type::cicada: return cicada;
        case creature::type::lizard: return lizard;
        case creature::type::daddy: return daddy;
        case creature::type::leech: return leeches[0];
        case creature::type::tube_worm: return tube_worm;
        case creature::type::count: break;
        default: return later[size_t(t)];
        }
        return centipede;
    }

    world_creatures spawn_creatures(sys::game_state& gs, dcon::room_id rid) {
        world_creatures out;
        auto const room = world::get_room_view(gs, rid);
        uint32_t const k0 = uint32_t(rid.index()) * 2654435761u;
        // the next lizard breed, daddy long legs, centipede and leech variant
        // in every room
        out.breed = lizard::breed(uint32_t(rid.index()) % lizard::breed_count);
        out.variant = daddy::variant(uint32_t(rid.index()) % daddy::variant_count);
        out.centipede_variant = centipede::variant(uint32_t(rid.index()) % centipede::variant_count);
        out.leech_variant = leech::variant(uint32_t(rid.index()) % leech::variant_count);
        for (auto const t : creature::all_types) {
            if (t == creature::type::slugcat)
                continue;
            // each kind rolls its spot from its own seed
            uint32_t salt = 0;
            switch (t) {
            case creature::type::slugcat: break;
            case creature::type::centipede: salt = 0xCE7712EDu; break;
            case creature::type::dropbug: salt = 0x51ED27u; break;
            case creature::type::cicada: salt = 0xC1CADAu; break;
            case creature::type::lizard: salt = 0x112A2Du; break;
            case creature::type::daddy: salt = 0xDADD1u; break;
            case creature::type::leech: salt = 0x1EEC4u; break;
            case creature::type::tube_worm: salt = 0x7B3E4u; break;
            case creature::type::count: break;
            default: salt = 0xA5170000u + uint32_t(t); break;
            }
            rw::rng rng{(k0 ^ salt) | 1u};
            auto& id = out.of(t);
            for (int k = 0; k < 50 && !id; ++k) {
                ivec2 const tile{rng.range(1, std::max(2, room.width - 1)), rng.range(1, std::max(2, room.height - 1))};
                creature::spawn_desc d{t};
                bool ok = false;
                switch (t) {
                case creature::type::slugcat:
                    break;
                case creature::type::centipede:
                    // somewhere it can stand (an aquapede in the water if
                    // there is any), any size
                    ok = pathing::accessible(room, tile, out.centipede_variant == centipede::variant::aqua ? pathing::aquacenti : pathing::climber);
                    if (ok) {
                        d.size = rng.value();
                        d.variety = uint8_t(out.centipede_variant);
                    }
                    break;
                case creature::type::dropbug:
                    // placed in the room's best ceiling spot like a story session does
                    ok = pathing::accessible(room, tile, pathing::drop_bug);
                    d.relocate = true;
                    break;
                case creature::type::cicada:
                    // somewhere in the air, either colour
                    ok = pathing::accessible(room, tile, pathing::cicada);
                    if (ok)
                        d.size = rng.value() < 0.5f ? 1.f : 0.f;
                    break;
                case creature::type::lizard:
                    // standing somewhere
                    ok = pathing::accessible(room, tile, lizard::params(out.breed).movement) && !room.get_tile(tile).solid();
                    d.variety = uint8_t(out.breed);
                    break;
                case creature::type::daddy:
                    // in the open
                    ok = pathing::accessible(room, tile, pathing::daddy) && pathing::terrain_proximity(room, tile) > 1;
                    d.variety = uint8_t(out.variant);
                    break;
                case creature::type::leech:
                    // under the water (a jungle leech on the ground)
                    ok = out.leech_variant == leech::variant::jungle ? pathing::accessible(room, tile, pathing::standard_ground) && !room.get_tile(tile).solid()
                                                                     : room.get_tile(tile).water == 1 && !room.get_tile(tile).solid();
                    d.variety = uint8_t(out.leech_variant);
                    break;
                case creature::type::tube_worm:
                    // on the ground
                    ok = pathing::accessible(room, tile, pathing::tube_worm) && !room.get_tile(tile).solid();
                    break;
                case creature::type::needle_worm:
                    ok = needle_worm::spawn_spot(room, tile);
                    d.variety = uint8_t(rng.value() < 0.5f ? needle_worm::variant::big : needle_worm::variant::small);
                    break;
                case creature::type::pole_mimic:
                    ok = pole_mimic::spawn_spot(room, tile);
                    break;
                case creature::type::tentacle_plant:
                    ok = tentacle_plant::spawn_spot(room, tile);
                    break;
                case creature::type::jetfish:
                    ok = jetfish::spawn_spot(room, tile);
                    break;
                case creature::type::scavenger:
                    ok = scavenger::spawn_spot(room, tile);
                    d.variety = uint8_t(std::min(uint32_t(rng.value() * float(scavenger::variant_count)), scavenger::variant_count - 1));
                    break;
                case creature::type::deer:
                    ok = deer::spawn_spot(room, tile);
                    break;
                case creature::type::miros:
                    ok = miros::spawn_spot(room, tile);
                    break;
                case creature::type::barnacle:
                    ok = barnacle::spawn_spot(room, tile);
                    break;
                case creature::type::egg_bug:
                    ok = egg_bug::spawn_spot(room, tile);
                    d.variety = uint8_t(rng.value() < 0.5f ? egg_bug::variant::egg_bug : egg_bug::variant::fire_bug);
                    break;
                case creature::type::garbage_worm:
                    ok = garbage_worm::spawn_spot(room, tile);
                    break;
                case creature::type::lantern_mouse:
                    ok = lantern_mouse::spawn_spot(room, tile);
                    break;
                case creature::type::loach:
                    ok = loach::spawn_spot(room, tile);
                    break;
                case creature::type::moth_grub:
                    ok = moth_grub::spawn_spot(room, tile);
                    break;
                case creature::type::angler:
                    ok = angler::spawn_spot(room, tile);
                    break;
                case creature::type::spider:
                    ok = spider::spawn_spot(room, tile);
                    break;
                case creature::type::big_spider:
                    ok = big_spider::spawn_spot(room, tile);
                    d.variety = uint8_t(rng.range(0, 3));
                    break;
                case creature::type::stowaway:
                    ok = stowaway::spawn_spot(room, tile);
                    break;
                case creature::type::drill_crab:
                    // (tower crabs need big rooms: the fuzz keeps to drill crabs)
                    ok = drill_crab::spawn_spot(room, tile);
                    d.variety = 0;
                    break;
                case creature::type::batfly:
                    ok = batfly::spawn_spot(room, tile);
                    break;
                case creature::type::frog:
                    ok = frog::spawn_spot(room, tile);
                    break;
                case creature::type::oracle:
                    // (the tools never spawn oracles)
                    break;
                case creature::type::inspector:
                    ok = inspector::spawn_spot(room, tile);
                    break;
                case creature::type::big_eel:
                    ok = big_eel::spawn_spot(room, tile);
                    break;
                case creature::type::moth:
                    ok = moth::spawn_spot(room, tile);
                    d.variety = uint8_t(rng.range(0, 2));
                    break;
                case creature::type::overseer:
                    ok = overseer::spawn_spot(room, tile);
                    break;
                case creature::type::snail:
                    ok = snail::spawn_spot(room, tile);
                    break;
                case creature::type::sky_whale:
                    ok = sky_whale::spawn_spot(room, tile);
                    break;
                case creature::type::yeek:
                    ok = yeek::spawn_spot(room, tile);
                    break;
                case creature::type::vulture:
                    ok = vulture::spawn_spot(room, tile);
                    d.variety = uint8_t(rng.range(0, int32_t(vulture::variant_count)));
                    break;
                case creature::type::count:
                    break;
                }
                if (ok && t == creature::type::leech) {
                    for (uint32_t i = 0; i < world_creatures::school; ++i)
                        out.leeches[i] = creature::create(gs, rid, world::room_view::middle_of_tile(tile) + vec2{float(i) * 6.f - 6.f, 0.f}, d);
                } else if (ok) {
                    id = creature::create(gs, rid, world::room_view::middle_of_tile(tile), d);
                }
            }
        }
        return out;
    }
}
