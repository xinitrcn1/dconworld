#pragma once

// Port of OracleGraphics: the arm (ArmJointGraphics: cogs, metal parts,
// the two bent segments of each joint as long meshes; ArmBase on the
// chamber's frame), the umbilical cord (UbilicalCord: an 80 point cord
// from the floor to the arm, with its 14 small cords to the head; Moon's
// DisconnectedUbilicalCord: 10 short ones off her head), the body chunks,
// neck, feet and hands (glyphs and long meshes), the gown (Gown: an 11 x 11
// cloth grid), the halo (Halo: two rings, the 20 connections that light up,
// the 100 memory bits) and the head (head, chin, eyes, the "phones", Moon's
// third eye, the past Moon's sigil), in vanilla's sprite and container
// order. The cords and the halo's rings and connections go through the
// rope pipeline.
//
// Not drawn: fadeSprite and killSprite (FlatLightBehindTerrain/FlatLight
// shaders), the LightSources (Pebbles' and the past Moon's), the
// MoonCloakTex texture of Moon's and Saint's Pebbles' gowns (flat colours).

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace oracle_graphics {
    // OracleGraphics' constructor (its layout rolled from Random.InitState(56
    // or 10544)) and the 100 updates of Oracle.Update's `initiated`
    void reset_positions(sys::game_state& state, dcon::creature_id c);
    // OracleGraphics.Update, once per tick after the oracle's
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // DrawSprites (ApplyPalette from `pal`)
    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip, std::vector<sprites::rope_point>& ropes);
}
