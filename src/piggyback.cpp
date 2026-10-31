#include <vector>

#include "piggyback.hpp"
#include "creature.hpp"
#include "items.hpp"
#include "player.hpp"
#include "rooms.hpp"

namespace piggyback {
    using rw::vec2;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // how close a slugcat has to be to take another on its back
        constexpr float reach = 40.f;

        vec2 pos(sys::game_state const& state, dcon::player_id p, uint32_t i) {
            return state.world.body_chunk_get_pos(player::body_chunk(state, p, i));
        }

        // ChangeOverlap: the link, made or undone
        void link(sys::game_state& state, dcon::player_id under, dcon::player_id over) {
            state.world.player_set_back_slug(under, over);
            state.world.player_set_on_back_of(over, under);
        }
        void unlink(sys::game_state& state, dcon::player_id under, dcon::player_id over) {
            state.world.player_set_back_slug(under, dcon::player_id{});
            state.world.player_set_on_back_of(over, dcon::player_id{});
        }

        bool close(sys::game_state const& state, dcon::player_id a, dcon::player_id b) {
            for (uint32_t i = 0; i < 2; ++i)
                for (uint32_t j = 0; j < 2; ++j)
                    if (custom::dist_less(pos(state, a, i), pos(state, b, j), reach))
                        return true;
            return false;
        }
    }

    dcon::player_id carrier(sys::game_state const& state, dcon::player_id p) noexcept {
        return state.world.player_get_on_back_of(p);
    }

    dcon::player_id rider(sys::game_state const& state, dcon::player_id p) noexcept {
        return state.world.player_get_back_slug(p);
    }

    dcon::player_id bottom(sys::game_state const& state, dcon::player_id p) noexcept {
        for (auto c = carrier(state, p); c; c = carrier(state, p))
            p = c;
        return p;
    }

    dcon::player_id top(sys::game_state const& state, dcon::player_id p) noexcept {
        for (auto r = rider(state, p); r; r = rider(state, p))
            p = r;
        return p;
    }

    uint32_t height(sys::game_state const& state, dcon::player_id p) noexcept {
        uint32_t n = 1;
        for (auto r = rider(state, bottom(state, p)); r; r = rider(state, r))
            ++n;
        return n;
    }

    bool mount(sys::game_state& state, dcon::player_id onto, dcon::player_id who) {
        auto const& w = state.world;
        if (!onto || !who || onto == who || riding(state, who) || bottom(state, onto) == who)
            return false;
        if (rooms::room_of(state, onto) != rooms::room_of(state, who) || player::in_shortcut(state, onto) || player::in_shortcut(state, who)
            || player::dead(state, onto) || player::dead(state, who) || w.player_get_eaten(who))
            return false;
        auto const under = top(state, onto);
        link(state, under, who);
        state.play_sound(sound::sound_id::slugcat_pick_up_creature, rooms::room_of(state, under), pos(state, under, 0));
        return true;
    }

    void drop(sys::game_state& state, dcon::player_id p) {
        auto& w = state.world;
        auto const r = rider(state, p);
        if (!r)
            return;
        unlink(state, p, r);
        // (off with a little push, its hips a bit higher)
        auto rng = rw::random(state.seed, state.tick, uint32_t(player::creature_of(state, r).value));
        auto const c0 = player::body_chunk(state, r, 0);
        auto const c1 = player::body_chunk(state, r, 1);
        w.body_chunk_set_vel(c0, w.body_chunk_get_vel(player::body_chunk(state, p, 0)) + custom::deg_to_vec(rng.value() * 360.f) * 3.f * rng.value());
        w.body_chunk_get_pos(c1).y += 10.f;
    }

    void undo(sys::game_state& state, dcon::player_id p) {
        auto c = bottom(state, p);
        while (auto const r = rider(state, c)) {
            drop(state, c);
            c = r;
        }
    }

    void scatter(sys::game_state& state, dcon::room_id room, vec2 at, float rad) {
        std::vector<dcon::player_id> hit;
        rooms::for_each_player(state, room, [&](dcon::player_id p) {
            if ((riding(state, p) || rider(state, p)) && !player::in_shortcut(state, p) && custom::dist_less(pos(state, p, 0), at, rad))
                hit.push_back(p);
        });
        for (auto const p : hit)
            undo(state, p);
    }

    bool player_input(sys::game_state& state, dcon::player_id p) {
        auto const& w = state.world;
        auto const& in = w.player_get_input(p);
        if (riding(state, p)) {
            if (in[0].jmp && !in[1].jmp)
                drop(state, carrier(state, p));
            return true;
        }
        if (!in[0].pckp || in[1].pckp || player::stun_of(state, p) > 0)
            return false;
        bool const free_hand = !items::held(state, p, 0) || !items::held(state, p, 1);
        // down: the one on its back off
        if (in[0].y < 0) {
            if (!rider(state, p) || !free_hand)
                return false;
            drop(state, p);
            return true;
        }
        if (!free_hand)
            return false;
        bool const pup = w.player_get_stats(p).name == player::slugcat_name::slugpup;
        dcon::player_id best;
        float best_d = 0.f;
        rooms::for_each_player(state, rooms::room_of(state, p), [&](dcon::player_id o) {
            if (o == p || player::in_shortcut(state, o) || player::dead(state, o) || !close(state, p, o))
                return;
            // (a pup it takes: one not on someone's back already, nor under it)
            if (!pup && (w.player_get_stats(o).name != player::slugcat_name::slugpup || riding(state, o) || bottom(state, p) == o))
                return;
            float const d = rw::distance(pos(state, p, 0), pos(state, o, 0));
            if (!best || d < best_d) {
                best = o;
                best_d = d;
            }
        });
        if (!best)
            return false;
        return pup ? mount(state, best, p) : mount(state, p, best);
    }

    void update_room(sys::game_state& state, world::room_view const& room) {
        auto& w = state.world;
        std::vector<dcon::player_id> bottoms;
        rooms::for_each_player(state, room.id, [&](dcon::player_id p) {
            if (!riding(state, p) && rider(state, p))
                bottoms.push_back(p);
        });
        for (auto const b : bottoms) {
            auto under = b;
            while (auto const r = rider(state, under)) {
                bool const apart = rooms::room_of(state, r) != room.id || player::in_shortcut(state, r) != player::in_shortcut(state, under);
                if (apart || player::dead(state, r) || player::dead(state, under) || w.player_get_eaten(r)
                    || w.physical_object_get_grabbed_by_count(player::body(state, r)) > 0) {
                    drop(state, under);
                    break;
                }
                if (player::in_shortcut(state, under)) {
                    under = r;
                    continue;
                }
                // its hips at the carrier's head (the carrier's graphics' head,
                // a rider's own main chunk), its front swinging over it
                auto const u0 = player::body_chunk(state, under, 0);
                vec2 const head = riding(state, under) ? w.body_chunk_get_pos(u0) + vec2{0.f, 6.f} : w.player_get_graphics(under).head.pos;
                auto const c0 = player::body_chunk(state, r, 0);
                auto const c1 = player::body_chunk(state, r, 1);
                vec2 const uv = w.body_chunk_get_vel(u0);
                w.body_chunk_set_pos(c1, head);
                w.body_chunk_set_vel(c1, uv);
                w.body_chunk_set_vel(c0, rw::lerp(w.body_chunk_get_vel(c0), rw::lerp(uv, vec2{0.f, 5.f}, 0.5f), 0.3f));
                under = r;
            }
        }
    }
}
