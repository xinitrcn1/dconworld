#include "sound_types.hpp"

namespace sound {
    char const* to_string(sound_id id) noexcept {
        static char const* const names[] = {
            "Slugcat_Terrain_Impact_Light",
            "Slugcat_Terrain_Impact_Medium",
            "Slugcat_Terrain_Impact_Hard",
            "Slugcat_Terrain_Impact_Death",
            "Slugcat_Floor_Impact_Standard",
            "Slugcat_Floor_Impact_Stealthy",
            "Slugcat_Roll_Init",
            "Slugcat_Normal_Jump",
            "Slugcat_Flip_Jump",
            "Slugcat_Wall_Jump",
            "Slugcat_Rocket_Jump",
            "Slugcat_Super_Jump",
            "Slugcat_Crouch_Jump",
            "Slugcat_From_Vertical_Pole_Jump",
            "Slugcat_From_Horizontal_Pole_Jump",
            "Slugcat_Belly_Slide_Init",
            "Slugcat_Throw_Rock",
            "Slugcat_Throw_Spear",
            "Slugcat_Pick_Up_Rock",
            "Slugcat_Pick_Up_Spear",
            "Slugcat_Lay_Down_Object",
            "Rock_Hit_Wall",
            "Spear_Stick_In_Wall",
            "Spear_Bounce_Off_Wall",
            "Spear_Stick_In_Ground",
            "Weapon_Skid",
            "Player_Tick_Along_In_Shortcut",
            "Gate_Poles_And_Rails_In",
            "Gate_Rails_Collide",
            "SM_Spear_Pull",
            "SM_Spear_Grab",
            "Slugcat_Stash_Spear_On_Back",
            "Rock_Hit_Creature",
            "Centipede_Shock",
            "Centipede_Attach",
        };
        static_assert(sizeof(names) / sizeof(names[0]) == size_t(sound_id::count));
        return id < sound_id::count ? names[size_t(id)] : "?";
    }
}
