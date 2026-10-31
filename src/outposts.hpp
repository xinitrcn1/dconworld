#pragma once

// The scavengers' loot without the scavengers: ScavengerOutpost (the toll
// spears it lays around itself, its antlers with their skulls and the
// PearlStrings of misc pearls hanging from them), ScavengerTreasury (spears,
// mostly explosive, bombs and pearls on the floor around it) and the story
// session's MultiplayerItem spawns. The guards, the toll itself (fees,
// passing, kill orders) and theft tracking aren't ported.

#include <cstdint>
#include <vector>

#include "gamestate.hpp"
#include "item_types.hpp"
#include "math.hpp"
#include "world.hpp"

namespace outposts {
    // DeerGraphics.Antlers(70, 0.7) of an outpost: rad and thickness
    inline constexpr float antler_rad = 70.f;
    inline constexpr float antler_thickness = 0.7f;

    // Room.Loaded's first pass over the placed objects, on the room's first
    // visit: ScavengerOutpost and ScavengerTreasury constructors,
    // Room.SpawnMultiplayerItem (in placed object order)
    void spawn_room(sys::game_state& state, dcon::room_id room);
    // PearlString.Update of the room's outposts (before the items move)
    void update_room(sys::game_state& state, world::room_view const& room);

    // DeerGraphics.Antlers(rad, ...) rolled from `r`, flattened part after
    // part into `out` (the rain deer's too); the number of parts
    int32_t generate_antlers(rw::unity_random& r, float rad, std::vector<items::antler_point>& out);
    // DeerGraphics.Antlers.Part.GetTransoformedPos
    rw::vec3 transformed_pos(items::antler_point const& p, float flip) noexcept;
    // DeerGraphics.Antlers.TransformToHeadRotat
    rw::vec2 transform_to_head_rotat(rw::vec3 dp, rw::vec2 antler_pos, float rotation, float flip, float face_dir) noexcept;
    // PearlString.AttachedPos: the antler tip the string hangs from
    rw::vec2 attached_pos(sys::game_state const& state, dcon::pearl_string_id s);
    // the item of a string's pearl while it is still that pearl, else invalid
    dcon::item_id pearl_item(sys::game_state const& state, items::string_pearl const& p) noexcept;
}
