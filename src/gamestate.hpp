#pragma once

// The whole game. Every object (regions, rooms, tiles, cameras, shortcuts,
// physical objects, body chunks, players...) lives in `world`, the data
// container. Whatever doesn't fit the SoA model (global constants, strings,
// lookup maps, the filesystem) lives next to it in the game_state.
//
// The data container stores its arrays inline (tens of MB), so a game_state
// must be heap allocated (see sys::make_game_state).

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "dcon_generated.hpp"
#include "layered_fs.hpp"
#include "pathing_types.hpp"
#include "sound_types.hpp"
#include "world_types.hpp"

namespace sys {
    struct game_state {
        dcon::data_container world;
        layered_fs::filesystem fs;

        // global constants
        // Room.gravity for rooms without a gravity effect
        float default_room_gravity = 1.f;
        // the simulation always runs at a fixed rate
        uint32_t ticks_per_second = RW_TICKS_PER_SECOND;
        // Rain World's screen, the size vanilla room cameras are laid out for
        float screen_width = 1366.f;
        float screen_height = 768.f;
        // simulation ticks since start
        uint32_t tick = 0;
        // UnityEngine.Random for world level rolls (item spawns...)
        rw::rng random{0x2545F491u};
        // sound/music requests of the last ticks, consumed by audio::update
        // (or dropped when nothing plays them)
        std::vector<sound::sound_request> sounds;
        std::vector<sound::music_request> music;
        // Room.InGameNoise of this tick, for whatever listens (the rot)
        struct noise {
            dcon::room_id room;
            rw::vec2 pos;
            float strength;
        };
        std::vector<noise> noises;

        // story progress the warps and the HUD care about (SaveState's
        // deathPersistentSaveData): karma 0..9 under its cap, and the
        // Watcher's ripple level 0..5 in half steps
        int32_t karma = 0;
        int32_t karma_cap = 4;
        float ripple_level = 1.f;
        // dynamic warps taken so far (seeds the next pick)
        uint32_t dynamic_warps_taken = 0;

        // RoomCamera.warpPointTimer: the whole screen warp transition, from
        // the warp point the Watcher stepped into to where it comes out
        struct warp_sequence {
            bool active = false;
            dcon::warp_point_id origin;
            dcon::room_id from_room;
            rw::vec2 from_pos{0.f, 0.f};
            dcon::room_id to_room;
            rw::vec2 to_pos{0.f, 0.f};
            float duration = 400.f;
            float progress = 0.f;
            float last_progress = 0.f;
            // the transfer happened (the second half plays in to_room)
            bool transferred = false;
            bool ripple = false;
            world::warp_effect effect;
            // objects the pull took hold of (objectsToRestoreGravityTo), they
            // travel along and get their gravity back on the other side
            std::vector<dcon::physical_object_id> pulled;
            std::vector<float> pulled_gravity;
        };
        warp_sequence warp;

        // RippleRing: expanding rings of the ripple layer (warp hints, the
        // Watcher's ability)
        struct ripple_ring {
            dcon::room_id room;
            rw::vec2 pos;
            float rad;       // final radius
            float life;      // 0..1
            float speed;     // life per tick
            float intensity;
        };
        std::vector<ripple_ring> ripple_rings;

        // Room.PlaySound
        void play_sound(sound::sound_id id, dcon::room_id room, rw::vec2 pos, float vol = 1.f, float pitch = 1.f) {
            if (sounds.size() < 256)
                sounds.push_back(sound::sound_request{id, room ? uint32_t(room.index()) : 0xFFFFFFFFu, pos, true, vol, pitch});
        }

        // pathing.hpp's distance fields: a cache derived from the rooms (it is
        // never saved), at most 32, recycled least recently used
        struct path_field {
            dcon::room_id room;
            rw::ivec2 goal{0, 0};
            pathing::movement movement;
            uint16_t geometry_version = 0;
            uint32_t last_used = 0;
            // travel cost to the goal per tile (row major), 0xFFFF unreachable
            std::vector<uint16_t> distance;
        };
        std::vector<path_field> path_fields;
        // Room.ceilingTiles of the rooms dropbugs sat in (dropbug.cpp), derived
        // from the tiles like the path fields and never saved
        struct ceiling_tiles {
            dcon::room_id room;
            uint16_t geometry_version = 0;
            std::vector<rw::ivec2> tiles;
        };
        std::vector<ceiling_tiles> ceiling_tile_cache;
        // AImap.terrainProximity of the rooms flying creatures are in
        // (pathing.cpp), derived like the path fields and never saved
        struct terrain_proximity_map {
            dcon::room_id room;
            uint16_t geometry_version = 0;
            std::vector<uint16_t> proximity; // row major
        };
        std::vector<terrain_proximity_map> terrain_proximity_cache;

        // cosmetic state, never saved (particles.hpp, water.hpp)
        struct particle {
            dcon::room_id room;
            uint8_t kind = 0;
            rw::vec2 pos{0.f, 0.f};
            rw::vec2 last_pos{0.f, 0.f};
            rw::vec2 vel{0.f, 0.f};
            float life = 0.f;   // 0..1
            float decay = 0.f;  // life per tick
            float size = 1.f;
            rw::vec3 color{1.f, 1.f, 1.f};
            float alpha = 1.f;
        };
        std::vector<particle> particles;
        // everything rains like an outdoor room at full rain intensity
        bool storm = false;
        // a room's water surface: a height field over x and depth (z, 0 at
        // the front), its waves, and the walls it reflects off
        struct water_surface {
            dcon::room_id room;
            float x0 = 0.f;
            float spacing = 6.f;
            uint32_t nx = 0;
            float level = 0.f;
            float time = 0.f;
            std::vector<float> h;       // nx * nz, row major in z
            std::vector<float> last_h;
            std::vector<float> v;
            std::vector<uint8_t> wall;  // per column: terrain at the surface
            uint32_t last_used = 0;
        };
        std::vector<water_surface> waters;
        // a room's sand (terrain.hpp): its ground height every few pixels,
        // built from the TerrainHandles and reshaped while playing (not saved)
        struct sand_field {
            dcon::room_id room;
            float x0 = 0.f;
            float spacing = 4.f;
            std::vector<float> h;    // current surface
            std::vector<float> back; // height of the dune's far side above it (the 3D band)
            float left = 0.f;        // the handles' span: no sand outside it
            float right = 0.f;
            float mean = 0.f;        // the sandstorm's resting level
            float amplitude = 0.f;   // and its dunes
            float phase = 0.f;
            uint32_t last_used = 0;
        };
        std::vector<sand_field> sands;


        // text storage, indexed by dcon::text_key
        std::vector<std::string> text;
        std::unordered_map<std::string, dcon::room_id> room_by_name;
        std::unordered_map<std::string, dcon::region_id> region_by_name;
        std::unordered_map<uint32_t, dcon::palette_id> palette_by_number;

        dcon::text_key add_text(std::string_view sv) {
            text.emplace_back(sv);
            return dcon::text_key(dcon::text_key::value_base_t(text.size() - 1));
        }
        std::string_view to_string_view(dcon::text_key k) const {
            return k ? std::string_view{text[size_t(k.index())]} : std::string_view{};
        }
        dcon::room_id find_room(std::string_view name) const;
        // the palette object for "palette<number>.png", created on first use
        dcon::palette_id palette_for(uint32_t number) {
            if (auto const it = palette_by_number.find(number); it != palette_by_number.end())
                return it->second;
            auto const pid = world.create_palette();
            world.palette_set_number(pid, uint16_t(number));
            palette_by_number.emplace(number, pid);
            return pid;
        }
    };

    std::unique_ptr<game_state> make_game_state();
}
