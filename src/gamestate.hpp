#pragma once

// The whole game. Every object (regions, rooms, tiles, cameras, shortcuts,
// physical objects, body chunks, players...) lives in `world`, the data
// container. Whatever doesn't fit the SoA model (global constants, strings,
// lookup maps, the filesystem) lives next to it in the game_state.
//
// The data container stores its arrays inline (tens of MB), so a game_state
// must be heap allocated (see sys::make_game_state).

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "creature_types.hpp"
#include "dcon_generated.hpp"
#include "layered_fs.hpp"
#include "pathing_types.hpp"
#include "sound_types.hpp"
#include "world_types.hpp"

namespace sys {
    // The objects of one kind in every room, sorted by id within a room (the
    // order dcon's for_each walks them in): rebuilt at the start of each
    // tick's parallel phase, when nothing changes rooms, so a room's update
    // finds its creatures without walking every creature of the world
    template<typename ID>
    struct room_buckets {
        std::vector<uint32_t> first; // per room, plus one past the end
        std::vector<ID> ids;
        std::span<ID const> in(dcon::room_id room) const noexcept {
            size_t const r = size_t(room.index());
            if (!room || r + 1 >= first.size())
                return {};
            return std::span<ID const>{ids.data() + first[r], ids.data() + first[r + 1]};
        }
        // buckets [from, to) of a bucketing with several buckets per room
        std::span<ID const> range(size_t from, size_t to) const noexcept {
            if (to >= first.size())
                return {};
            return std::span<ID const>{ids.data() + first[from], ids.data() + first[to]};
        }
    };
    struct room_index {
        // only while the rooms update in parallel (simulation.cpp); the
        // helpers of rooms.hpp walk every object of the kind otherwise
        bool valid = false;
        room_buckets<dcon::player_id> players;
        // creatures by room and kind: kind `t` of room `r` is bucket
        // r * creature::type_count + t, so a room's are kind by kind
        room_buckets<dcon::creature_id> creatures;
        room_buckets<dcon::item_id> items;
        room_buckets<dcon::lizard_spit_id> spits;
    };

    struct game_state;

    // Room.InGameNoise, for whatever listens (the rot, lizards, daddies...)
    struct noise {
        dcon::room_id room;
        rw::vec2 pos;
        float strength;
        // InGameNoise.sourceObject, when it is a physical object
        dcon::physical_object_id source{};
    };

    // pathing.hpp's distance fields: a cache derived from the room (it is
    // never saved), a handful per room, recycled least recently used
    struct path_field {
        rw::ivec2 goal{0, 0};
        pathing::movement movement;
        uint16_t geometry_version = 0;
        uint32_t last_used = 0;
        // travel cost to the goal per tile (row major), 0xFFFF unreachable
        std::vector<uint16_t> distance;
    };

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
        // a CentipedeShell: its spin in the picture plane and around its
        // length, its scale, hue and saturation, and its counters
        float rotation = 0.f, last_rotation = 0.f, rot_vel = 0.f;
        float z_rotation = 0.f, last_z_rotation = 0.f, z_rot_vel = 0.f;
        rw::vec2 scale{1.f, 1.f};
        float hue = 0.f, saturation = 0.f;
        int32_t counter = 0, disappear = 0;
    };
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
    };
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
    };
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

    // What a room keeps outside the data container: caches derived from its
    // tiles, its cosmetic simulation (water, sand), and what its update
    // produced this tick. Rooms update in parallel, each one touching only its
    // own room_runtime; the per tick outputs are merged into the game_state's
    // queues afterwards, in room order
    struct room_runtime {
        // pathing.cpp: distance fields, AImap.terrainProximity, and dropbug.cpp's
        // Room.ceilingTiles, all rebuilt when the geometry_version changes
        std::vector<path_field> path_fields;
        std::vector<uint16_t> proximity; // row major, empty until asked for
        uint16_t proximity_version = 0;
        std::vector<uint8_t> access; // pathing::access per tile, row major
        uint16_t access_version = 0;
        std::vector<rw::ivec2> ceiling_tiles;
        std::optional<uint16_t> ceiling_version;
        std::optional<water_surface> water;
        // built on first use (an empty height map: no sand in the room)
        std::optional<sand_field> sand;
        // a camera looks at it this tick: graphics modules, particles,
        // sounds and music only run for these (simulation.cpp)
        bool viewed = false;
        bool was_viewed = false;
        // what Room.Loaded does on the first visit was done (at startup for
        // every room, simulation::populate)
        bool populated = false;
        // the room's own generator for cosmetic rolls (particles)
        rw::rng cosmetic{0x51A5C0DEu};
        // this tick's outputs
        std::vector<noise> noises;
        std::vector<sound::sound_request> sounds;
        std::vector<sound::loop_request> loops;
        std::vector<sound::music_request> music;
        std::vector<particle> particles;
        std::vector<ripple_ring> ripple_rings;
        // what the room's update can't do while the other rooms update:
        // creating and deleting objects, moving them to other rooms. Applied
        // after the parallel phase, room by room in order, each room's in
        // the order they were queued (the "transactions")
        std::vector<std::function<void(game_state&)>> deferred;
    };

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
        // UnityEngine.Random for world level rolls: the world seed every
        // room's own generator starts from (simulation::populate), and the
        // creatures' generators as they are made (always serially). Never
        // used while the rooms update: they roll with room.random
        rw::rng random{0x2545F491u};
        // RainWorldGame.nextIssuedId: the EntityID numbers handed out so far
        int32_t next_issued_id = 0;
        // the campaign being played (SlugcatStats.Timeline): room spawns that
        // depend on it read this, never a player in the room
        ::player::slugcat_name story_slugcat = ::player::slugcat_name::white;
        // sound/music requests of the last ticks, consumed by audio::update
        // (or dropped when nothing plays them). Only the viewed rooms make
        // any
        std::vector<sound::sound_request> sounds;
        std::vector<sound::music_request> music;
        // the loops the viewed rooms keep up this tick (sound::loop_request),
        // in room order: replaced every tick, `loop_tick` counts the ticks
        // that made them (the audio side reconciles its voices once per new
        // tick, and leaves them alone while the game is paused)
        std::vector<sound::loop_request> loops;
        uint32_t loop_tick = 0;
        using noise = sys::noise;

        // per room state outside the data container, indexed by room
        std::vector<room_runtime> rooms;
        // the objects of every room while they update in parallel
        room_index index;
        // the rooms are updating in parallel (simulation.cpp): structural
        // changes are deferred, outputs go to the room's buffers
        bool parallel = false;
        // rooms a camera looks at besides the players' (the app's view)
        std::vector<dcon::room_id> camera_rooms;

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

        using ripple_ring = sys::ripple_ring;
        std::vector<ripple_ring> ripple_rings;

        room_runtime& runtime(dcon::room_id room) { return rooms[size_t(room.index())]; }
        room_runtime const& runtime(dcon::room_id room) const { return rooms[size_t(room.index())]; }
        bool has_runtime(dcon::room_id room) const noexcept { return room && size_t(room.index()) < rooms.size(); }
        // a camera looks at the room: sounds, music, particles and graphics
        // modules are only for these
        bool viewed(dcon::room_id room) const noexcept { return has_runtime(room) && rooms[size_t(room.index())].viewed; }
        // the room's UnityEngine.Random
        rw::rng& room_random(dcon::room_id room) { return world.room_get_random(room); }

        // Room.PlaySound: heard only from the rooms a camera looks at
        void play_sound(sound::sound_id id, dcon::room_id room, rw::vec2 pos, float vol = 1.f, float pitch = 1.f) {
            sound::sound_request const r{id, room ? uint32_t(room.index()) : 0xFFFFFFFFu, pos, true, vol, pitch};
            if (has_runtime(room)) {
                auto& rt = rooms[size_t(room.index())];
                if (!rt.viewed)
                    return;
                if (parallel) {
                    if (rt.sounds.size() < 256)
                        rt.sounds.push_back(r);
                    return;
                }
            } else if (parallel) {
                return;
            }
            if (sounds.size() < 256)
                sounds.push_back(r);
        }
        // A loop's upkeep for this tick (DynamicSoundLoop.Update, an emitter's
        // alive = true): only from the rooms a camera looks at, and silent
        // loops (volume 0) stop like vanilla's
        void play_loop(sound::loop_request const& r) {
            if (!(r.vol > 0.f))
                return;
            dcon::room_id const room{dcon::room_id::value_base_t(r.room)};
            if (r.room == 0xFFFFFFFFu || !has_runtime(room) || !rooms[size_t(room.index())].viewed)
                return;
            auto& out = parallel ? rooms[size_t(room.index())].loops : loops;
            if (out.size() < 256)
                out.push_back(r);
        }
        // ChunkSoundEmitter/StaticSoundLoop: a loop at `pos`
        void play_loop(uint64_t owner, sound::sound_id id, dcon::room_id room, rw::vec2 pos, float vol, float pitch = 1.f, uint16_t fade_out_frames = 0) {
            sound::loop_request r;
            r.owner = owner;
            r.id = id;
            r.room = room ? uint32_t(room.index()) : 0xFFFFFFFFu;
            r.pos = pos;
            r.vol = vol;
            r.pitch = pitch;
            r.fade_out_frames = fade_out_frames;
            play_loop(r);
        }
        // Room.InGameNoise: heard in the room the rest of this tick
        void add_noise(dcon::room_id room, rw::vec2 pos, float strength, dcon::physical_object_id source = {}) {
            if (!has_runtime(room))
                return;
            auto& n = rooms[size_t(room.index())].noises;
            if (n.size() < 64)
                n.push_back(noise{room, pos, strength, source});
        }
        std::span<noise const> noises(dcon::room_id room) const {
            if (!has_runtime(room))
                return {};
            return rooms[size_t(room.index())].noises;
        }
        // A structural change made from a room's update (creating, deleting
        // or moving objects between rooms): applied right away, or after
        // the parallel phase when the rooms are updating in parallel
        template<typename F>
        void defer(dcon::room_id room, F&& f) {
            if (parallel && has_runtime(room))
                rooms[size_t(room.index())].deferred.emplace_back(std::forward<F>(f));
            else
                f(*this);
        }

        // cosmetic particles of the viewed rooms (particles.hpp)
        using particle = sys::particle;
        std::vector<particle> particles;
        // everything rains like an outdoor room at full rain intensity
        bool storm = false;
        using water_surface = sys::water_surface;
        using sand_field = sys::sand_field;

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
