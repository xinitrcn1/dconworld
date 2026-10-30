#include <cmath>

#include "explosions.hpp"
#include "creature.hpp"
#include "particles.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "rooms.hpp"
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
        // and what lives is hurt
        creature::blast(state, room.id, at, d.rad, d.force, d.damage, d.stun, d.kill_rad, source);
        state.play_sound(sound::sound_id::fire_spear_explode, room.id, at);
        state.add_noise(room.id, at, 8000.f);
        if (room.water && room.point_submerged(at))
            water::disturb(state, room.id, at.x, 4.f + d.force, d.rad * 0.7f);

        // the look of it (rolled from the room's cosmetic generator, only
        // where a camera looks)
        if (!state.viewed(room.id))
            return;
        auto& r = state.runtime(room.id).cosmetic;
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
        if (room.water && room.point_submerged(at))
            particles::bubbles(state, room.id, at, 40, d.rad * 0.3f);
    }

    void flash(sys::game_state& state, world::room_view const& room, vec2 at, float rad) {
        // FlareBomb: whatever sees the flash is blinded for a while
        creature::flash(state, room, at, rad);
        particles::spawn(state, room.id, particles::kind::flash, at, vec2{0.f, 0.f}, rad * 0.35f, 1.2f, rw::vec3{1.f, 1.f, 0.95f}, 1.f);
        particles::spawn(state, room.id, particles::kind::flash, at, vec2{0.f, 0.f}, rad * 0.12f, 0.6f, rw::vec3{1.f, 1.f, 1.f}, 1.f);
        state.play_sound(sound::sound_id::fire_spear_explode, room.id, at, 0.4f, 1.6f);
    }
}
