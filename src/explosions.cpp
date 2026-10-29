#include <cmath>

#include "explosions.hpp"
#include "centipede.hpp"
#include "cicada.hpp"
#include "dropbug.hpp"
#include "lizard.hpp"
#include "daddy.hpp"
#include "particles.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "water.hpp"

namespace explosions {
    using rw::vec2;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    void explode(sys::game_state& state, world::room_view const& room, vec2 at, desc const& d, dcon::physical_object_id source) {
        auto& w = state.world;
        // everything close is thrown back
        w.room_for_each_object_location(room.id, [&](dcon::object_location_id l) {
            auto const o = w.object_location_get_object(l);
            if (o == source)
                return;
            w.physical_object_for_each_chunk_ownership_as_owner(o, [&](dcon::chunk_ownership_id co) {
                auto const c = w.chunk_ownership_get_chunk(co);
                vec2 const cp = w.body_chunk_get_pos(c);
                float const dist = rw::distance(cp, at);
                if (dist >= d.rad || !room.visual_contact(at, cp))
                    return;
                float const k = 1.f - dist / d.rad;
                w.body_chunk_get_vel(c) += custom::dir_vec(at, cp) * d.force * k / mathf::max(0.3f, w.body_chunk_get_mass(c));
            });
        });
        w.for_each_centipede([&](dcon::centipede_id c) {
            if (w.physical_object_get_room_from_object_location(centipede::body(state, c)) != room.id)
                return;
            uint32_t const n = centipede::chunk_count(state, c);
            for (uint32_t i = 0; i < n; ++i) {
                vec2 const cp = w.body_chunk_get_pos(centipede::chunk(state, c, i));
                float const dist = rw::distance(cp, at);
                if (dist < d.rad) {
                    float const k = 1.f - dist / d.rad;
                    centipede::violence(state, c, i, custom::dir_vec(at, cp) * d.force * k, d.damage * k, d.stun * k);
                    break;
                }
            }
        });
        w.for_each_dropbug([&](dcon::dropbug_id b) {
            if (w.physical_object_get_room_from_object_location(dropbug::body(state, b)) != room.id)
                return;
            for (uint32_t i = 0; i < 3; ++i) {
                vec2 const cp = w.body_chunk_get_pos(dropbug::chunk(state, b, i));
                float const dist = rw::distance(cp, at);
                if (dist < d.rad) {
                    float const k = 1.f - dist / d.rad;
                    dropbug::violence(state, b, i, custom::dir_vec(at, cp) * d.force * k, d.damage * k, d.stun * k);
                    break;
                }
            }
        });
        w.for_each_cicada([&](dcon::cicada_id c) {
            if (w.physical_object_get_room_from_object_location(cicada::body(state, c)) != room.id)
                return;
            for (uint32_t i = 0; i < 2; ++i) {
                vec2 const cp = w.body_chunk_get_pos(cicada::chunk(state, c, i));
                float const dist = rw::distance(cp, at);
                if (dist < d.rad) {
                    float const k = 1.f - dist / d.rad;
                    cicada::violence(state, c, i, custom::dir_vec(at, cp) * d.force * k, d.damage * k, d.stun * k);
                    break;
                }
            }
        });
        w.for_each_lizard([&](dcon::lizard_id l) {
            if (w.physical_object_get_room_from_object_location(lizard::body(state, l)) != room.id)
                return;
            for (uint32_t i = 0; i < 3; ++i) {
                vec2 const cp = w.body_chunk_get_pos(lizard::chunk(state, l, i));
                float const dist = rw::distance(cp, at);
                if (dist < d.rad) {
                    float const k = 1.f - dist / d.rad;
                    lizard::violence(state, l, i, custom::dir_vec(at, cp) * d.force * k, d.damage * k, d.stun * k, false, true);
                    break;
                }
            }
        });
        w.for_each_daddy([&](dcon::daddy_id dl) {
            if (w.physical_object_get_room_from_object_location(daddy::body(state, dl)) != room.id)
                return;
            uint32_t const n = daddy::chunk_count(state, dl);
            for (uint32_t i = 0; i < n; ++i) {
                vec2 const cp = w.body_chunk_get_pos(daddy::chunk(state, dl, i));
                float const dist = rw::distance(cp, at);
                if (dist < d.rad) {
                    float const k = 1.f - dist / d.rad;
                    daddy::violence(state, dl, daddy::hit_part{int32_t(i)}, custom::dir_vec(at, cp) * d.force * k, d.damage * k, d.stun * k, true);
                    break;
                }
            }
        });
        if (d.kill_rad > 0.f) {
            w.for_each_player([&](dcon::player_id p) {
                auto const o = player::body(state, p);
                if (o == source || w.physical_object_get_room_from_object_location(o) != room.id || w.player_get_in_shortcut(p) || w.player_get_dead(p))
                    return;
                vec2 const cp = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
                if (rw::custom::dist_less(cp, at, d.kill_rad))
                    player::die(state, p);
                else if (rw::custom::dist_less(cp, at, d.rad))
                    player::stun(state, p, int32_t(d.stun * (1.f - rw::distance(cp, at) / d.rad)));
            });
        }
        state.play_sound(sound::sound_id::fire_spear_explode, room.id, at);
        if (state.noises.size() < 64)
            state.noises.push_back({room.id, at, 8000.f});

        // the look of it
        static rw::rng r{0xB0B0CAFEu};
        particles::spawn(state, room.id, particles::kind::flash, at, vec2{0.f, 0.f}, d.rad * 0.45f, 0.35f, rw::vec3{1.f, 0.95f, 0.85f}, 1.f);
        particles::spawn(state, room.id, particles::kind::flash, at, vec2{0.f, 0.f}, d.rad * 0.2f, 0.2f, rw::vec3{1.f, 1.f, 1.f}, 1.f);
        for (int i = 0; i < 24; ++i) {
            vec2 const dir = custom::deg_to_vec(r.value() * 360.f);
            particles::spawn(state, room.id, particles::kind::spark, at + dir * r.value() * 20.f, dir * mathf::lerp(5.f, 18.f, r.value()), 1.8f, 0.35f, rw::vec3{1.f, 0.85f, 0.55f});
        }
        for (int i = 0; i < 10; ++i) {
            vec2 const dir = custom::deg_to_vec(r.value() * 360.f);
            particles::spawn(state, room.id, particles::kind::dust, at + dir * r.value() * 15.f, dir * mathf::lerp(1.f, 4.f, r.value()), 8.f, 1.2f, rw::vec3{0.3f, 0.28f, 0.27f}, 0.5f);
        }
        if (room.water && room.point_submerged(at)) {
            particles::bubbles(state, room.id, at, 40, d.rad * 0.3f);
            water::disturb(state, room.id, at.x, 4.f + d.force, d.rad * 0.7f);
        }
    }

    void flash(sys::game_state& state, world::room_view const& room, vec2 at, float rad) {
        auto& w = state.world;
        // FlareBomb: whatever sees the flash is blinded for a while
        w.for_each_centipede([&](dcon::centipede_id c) {
            if (w.physical_object_get_room_from_object_location(centipede::body(state, c)) != room.id)
                return;
            vec2 const cp = w.body_chunk_get_pos(centipede::chunk(state, c, centipede::head_index(state, c)));
            if (rw::custom::dist_less(cp, at, rad) && room.visual_contact(at, cp))
                centipede::violence(state, c, centipede::head_index(state, c), vec2{0.f, 0.f}, 0.f, 120.f);
        });
        w.for_each_dropbug([&](dcon::dropbug_id b) {
            if (w.physical_object_get_room_from_object_location(dropbug::body(state, b)) != room.id)
                return;
            vec2 const cp = w.body_chunk_get_pos(dropbug::chunk(state, b, 0));
            if (rw::custom::dist_less(cp, at, rad) && room.visual_contact(at, cp))
                dropbug::violence(state, b, 0, vec2{0.f, 0.f}, 0.f, 120.f);
        });
        w.for_each_cicada([&](dcon::cicada_id c) {
            if (w.physical_object_get_room_from_object_location(cicada::body(state, c)) != room.id)
                return;
            vec2 const cp = w.body_chunk_get_pos(cicada::chunk(state, c, 0));
            if (rw::custom::dist_less(cp, at, rad) && room.visual_contact(at, cp))
                cicada::violence(state, c, 0, vec2{0.f, 0.f}, 0.f, 120.f);
        });
        w.for_each_lizard([&](dcon::lizard_id l) {
            if (w.physical_object_get_room_from_object_location(lizard::body(state, l)) != room.id)
                return;
            vec2 const cp = w.body_chunk_get_pos(lizard::chunk(state, l, 0));
            if (rw::custom::dist_less(cp, at, rad) && room.visual_contact(at, cp))
                lizard::violence(state, l, 0, vec2{0.f, 0.f}, 0.f, 120.f);
        });
        particles::spawn(state, room.id, particles::kind::flash, at, vec2{0.f, 0.f}, rad * 0.35f, 1.2f, rw::vec3{1.f, 1.f, 0.95f}, 1.f);
        particles::spawn(state, room.id, particles::kind::flash, at, vec2{0.f, 0.f}, rad * 0.12f, 0.6f, rw::vec3{1.f, 1.f, 1.f}, 1.f);
        state.play_sound(sound::sound_id::fire_spear_explode, room.id, at, 0.4f, 1.6f);
    }
}
