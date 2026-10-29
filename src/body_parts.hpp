#pragma once

// GraphicsModule body parts shared by the creature graphics ports: BodyPart,
// GenericBodyPart and Limb, over player::body_part / player::hand_state.

#include "math.hpp"
#include "player_types.hpp"
#include "world.hpp"

namespace body_parts {
    // Limb constructor arguments
    struct limb_params {
        float rad;
        float surface_friction;
        float air_friction;
        float hunt_speed; // defaultHuntSpeed
        float quickness;  // defaultQuickness
    };

    // BodyPart.ConnectToPoint
    void connect_to_point(player::body_part& bp, rw::vec2 pnt, float connection_rad, bool push, float elastic, rw::vec2 host_vel, float adapt, float exaggerate);
    // BodyPart.PushOutOfTerrain (tile terrain only): whether it touched the
    // terrain (BodyPart.terrainContact)
    bool push_out_of_terrain(player::body_part& bp, world::room_view const& room, float surface_friction);
    // GenericBodyPart.Update
    void generic_update(player::body_part& bp, world::room_view const& room, float air_friction, float surface_friction);
    // Custom.RotateAroundOrigo (clockwise degrees)
    rw::vec2 rotate_around_origo(rw::vec2 v, float deg);
    // Limb.FindGrip: the closest point on terrain or a beam to `goal`
    void find_grip(player::hand_state& h, world::room_view const& room, rw::vec2 attached, rw::vec2 search_from, float max_rad, rw::vec2 goal,
        int forbidden_x, int forbidden_y, bool behind_walls = false);
    // Limb.Update; `conn`, `conn_vel` are the connection chunk's, `rotation_chunk` its rotationChunk's position
    void limb_update(player::hand_state& h, limb_params const& lp, rw::vec2 conn, rw::vec2 conn_vel, rw::vec2 rotation_chunk, world::room_view const& room);

    // a limb as a body part of radius `rad`, for the helpers above
    template<typename F>
    void as_body_part(player::hand_state& h, float rad, F&& f) {
        player::body_part bp;
        bp.pos = h.pos;
        bp.last_pos = h.last_pos;
        bp.vel = h.vel;
        bp.rad = rad;
        f(bp);
        h.pos = bp.pos;
        h.last_pos = bp.last_pos;
        h.vel = bp.vel;
    }
}
