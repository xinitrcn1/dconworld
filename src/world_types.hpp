#pragma once

// Plain types stored in the data container (see dcon_generated.txt)

#include <cstdint>
#include <string>

#include "math.hpp"

namespace world {
    // World.preProcessingGeneration: connectivity maps older than this are stale
    inline constexpr int32_t preprocessing_generation = 20;

    // Room.Tile.TerrainType
    enum class terrain : uint8_t {
        air,
        solid,
        slope,
        floor,
        shortcut_entrance,
    };

    // Room.Tile boolean attributes
    enum tile_flag : uint8_t {
        tile_vertical_beam = 1 << 0,
        tile_horizontal_beam = 1 << 1,
        tile_wall_behind = 1 << 2,
        tile_hive = 1 << 3,
        tile_waterfall = 1 << 4,
        tile_garbage_hole = 1 << 5,
        tile_worm_grass = 1 << 6,
    };

    // Room.SlopeDirection
    enum class slope_direction : uint8_t {
        up_left,
        up_right,
        down_left,
        down_right,
        broken,
    };

    // ShortcutData.Type
    enum class shortcut_type : uint8_t {
        normal,
        room_exit,
        creature_hole,
        npc_transportation,
        region_transportation,
        dead_end,
    };

    enum class item_type : uint8_t {
        rock,
        spear,
        explosive_spear, // ExplosiveSpear: goes off when it hits
        electric_spear,  // ElectricSpear: shocks what it hits while charged
        bomb,            // ScavengerBomb: explodes on impact
        flare_bomb,      // FlareBomb: a blinding flash on impact
        lilly_puck,      // LillyPuck: a disc weapon that cuts
        mushroom,
        slime_mold,      // the glowing orange mold
        dangle_fruit,
        rotten_fruit,    // Watcher's RottenDangleFruit
        data_pearl,
        count
    };

    // Weapon.Mode
    enum class weapon_mode : uint8_t {
        free,
        thrown,
        carried,
        stuck_in_wall,
        on_back,
    };

    // RegionGate.Mode
    enum class gate_mode : uint8_t {
        middle_closed,
        closing_air_lock,
        waiting,
        opening_middle,
        middle_open,
        closing_middle,
        opening_side,
        closed,
        broken,
    };

    // two bytes, indexable (DataContainer type names can't hold commas)
    struct byte_pair {
        uint8_t v[2] = {0, 0};
        uint8_t& operator[](size_t i) noexcept { return v[i]; }
        uint8_t operator[](size_t i) const noexcept { return v[i]; }
    };

    // CentipedeAI.Behavior
    enum class centipede_behavior : uint8_t {
        idle,
        flee,
        hunt,
        injured,
        escape_rain,
    };

    // RoomSettings.DangerType
    enum class danger_type : uint8_t {
        rain,
        flood,
        flood_and_rain,
        none,
        // Downpour's and the Watcher's: the Aerie's blizzard rains like Rain
        // (RoomRain pushes with the global rain at full strength), the rest
        // have no RoomRain at all
        aerie_blizzard,
        blizzard,
        sandstorm,
        thunder,
        unknown,
    };

    // AmbientSound.Type
    enum class ambient_type : uint8_t {
        omni,        // OmniDirectionalSound: everywhere in the room, centred
        directional, // DirectionalSound: panned by its direction
        spot,        // SpotSound: around a point, fading out to its taper radius
    };
    // one entry of a settings file's "AmbientSounds:" line
    struct ambient_sound_desc {
        ambient_type type = ambient_type::omni;
        std::string sample; // as written, with its extension
        float volume = 0.5f;
        float pitch = 1.f;
        rw::vec2 pos{0.f, 0.f};          // spot
        rw::vec2 direction{0.f, -1.f};   // directional
        float rad = 50.f;                // spot
        float taper = 0.1f;              // spot
        float doppler = 0.f;
    };

    // Rot (DaddyCorruption) placed objects of the room settings
    enum class rot_kind : uint8_t {
        corruption,         // Corruption: a rot area with hostile bulbs
        passive_corruption, // PassiveCorruption: an area with CustomRotData
        tube,               // CorruptionTube: a climbable rot vine
        darkness,           // CorruptionDarkness: a black goo blot
    };

    // DaddyCorruption.CustomRotData.RotColor
    enum class rot_color : uint8_t {
        default_,
        effect_color_a,
        effect_color_b,
    };

    // DaddyCorruption.CustomRotData (PassiveCorruption), vanilla defaults
    struct rot_custom_data {
        float min_size = 7.f;
        float max_size = 15.f;
        float eye_chance = 0.8f;
        float min_eye_size = 0.3f;
        float max_eye_size = 0.55f;
        float min_color = 0.05f;
        float max_color = 0.15f;
        float slowdown = 0.2f;
        float leg_chance = 0.2f;
        float density = 2.f;
        float darkness_chance = 0.f;
        float darkness_scale = 1.f;
        rot_color color = rot_color::default_;
        bool cross_eyes = false; // EyeType.Cross, else Dot
        bool hostile = false;    // Behavior.Hostile, else Passive
    };

    // the CorruptionTube subclasses
    enum class rot_tube_kind : uint8_t {
        leg,     // LittleLeg: grabs prey and pulls it into the rot
        tendril, // Tendril: a passive strand hanging from a bulb
        climb,   // ClimbableCorruptionTube: a vine stretched between two walls
    };

    // CorruptionTube.segments[i, 0..2]
    struct rot_segment {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
    };
    // RopeGraphic.Segment (drawn positions of a tube)
    struct rot_graphic_segment {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
    };
    // TubeGraphic.Bump: pos.x across the tube, pos.y along it (0..1)
    struct rot_bump {
        rw::vec2 pos{0.f, 0.f};
        float size = 0.f;
        float eye_size = 0.f;
    };

    // Rope (a line wrapping around tile corners); bends are capped
    struct rope_corner {
        rw::vec2 pos{0.f, 0.f};
        uint8_t dir = 0; // FloatRect.CornerLabel A (top left), B, C, D
    };
    struct rope_state {
        static constexpr uint32_t max_bends = 8;
        rw::vec2 a{0.f, 0.f};
        rw::vec2 b{0.f, 0.f};
        rw::vec2 last_a{0.f, 0.f};
        rw::vec2 last_b{0.f, 0.f};
        float total_length = 0.f;
        float thickness = 2.f;
        uint8_t bend_count = 0;
        rope_corner bends[max_bends];
    };

    // the bulbs of one tile of a rot system (DaddyCorruption.bulbs[x, y]):
    // bulbs of a tile are created together, so they are contiguous
    struct rot_cell {
        uint32_t first = 0;
        uint16_t count = 0;
    };

    // a creature body chunk held by the rot (a slugcat's included): `id`
    // the creature's index
    enum class prey_kind : uint8_t {
        none,
        creature,
    };
    struct rot_prey {
        prey_kind kind = prey_kind::none;
        uint8_t chunk = 0;
        uint16_t id = 0;
        constexpr bool valid() const noexcept { return kind != prey_kind::none; }
        constexpr bool same_creature(rot_prey o) const noexcept { return kind == o.kind && id == o.id; }
    };

    // WarpPoint.WarpPointData.EffectSettings: 4 bit cosmetic knobs of the
    // warp transition, and how long triggering and activating take
    struct warp_effect {
        uint8_t vignette = 0;
        uint8_t darkness = 6;
        uint8_t spiral_gap = 8;
        uint8_t swirl = 4;
        uint8_t lensing = 8;
        uint8_t noise = 4;
        uint8_t twist = 4;
        uint8_t agitation = 4;
        uint8_t spaghettification = 7;
        uint16_t trigger_duration = 150;
        uint16_t active_duration = 150;
    };

    // WarpPoint.State
    enum class warp_state : uint8_t {
        ready,       // ReadyForWarp: waiting for the Watcher to stand in it
        enter,       // EnterWarp: pulling everything in
        exit,        // ExitWarp: the Watcher arrived here, floating out
        spawn_items, // SpawnItems
        cool_down,   // CoolDown
        closed,      // Closed: used up
    };

    // the placed objects that become items or plants (DangleFruit, Mushroom,
    // SlimeMold, SeedCob, LillyPuck, DataPearl, FlareBomb, MudPit, TerrainHandle...)
    enum class thing_kind : uint8_t {
        dangle_fruit,
        rotten_fruit,
        mushroom,
        slime_mold,
        seed_cob,
        dead_seed_cob,
        rotten_seed_cob,
        lilly_puck,
        data_pearl,
        unique_pearl,
        flare_bomb,
        mud_pit,        // handle: the pit's size, extra: its depth
        terrain_handle, // handle: left tangent, handle2: right tangent, extra: back height
        // the scavengers' loot (without the scavengers)
        scav_outpost,     // handle: the pole (handlePos), handle2: (skullSeed, pearlsSeed), extra: direction
        scav_treasury,    // handle: the radius handle
        reliable_spear,   // ReliableSpear: always a spear
        multiplayer_item, // MultiplayerItem: name the item type, extra the chance
    };

    // One entry of AbstractRoomNode.connectivity[template, node]
    struct node_connectivity {
        int16_t distance = -1;
        int16_t second = -1;
    };

    // By-value copy of a Room.Tile, assembled from the tile properties
    struct tile {
        terrain t = terrain::air;
        uint8_t flags = 0;
        uint8_t shortcut = 0; // Room.Tile.shortCut
        uint8_t water = 0;    // Room.Tile.waterInt (0 none, 1 deep, 2 surface)

        constexpr bool solid() const noexcept { return t == terrain::solid; }
        constexpr bool vertical_beam() const noexcept { return (flags & tile_vertical_beam) != 0; }
        constexpr bool horizontal_beam() const noexcept { return (flags & tile_horizontal_beam) != 0; }
        constexpr bool any_beam() const noexcept { return (flags & (tile_vertical_beam | tile_horizontal_beam)) != 0; }
    };
}
