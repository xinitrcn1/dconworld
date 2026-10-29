#pragma once

// Plain data of the items that doesn't fit a single property: the explosive
// spear's rag and the electric spear's flux (weapon_extra), the data pearls'
// glimmer, and the scavenger outposts' antlers and pearl strings (outposts.*).

#include <cstdint>

#include "math.hpp"

namespace items {
    // ExplosiveSpear.rag: [i, 0] pos, [1] last pos, [2] vel, [3] and [4] the
    // twist (and its last value), [5] the twist's drift
    inline constexpr int32_t max_rag = 9;
    struct rag_segment {
        rw::vec2 v[6];
    };
    struct spear_extra {
        // ExplosiveSpear
        rag_segment rag[max_rag];
        uint8_t rag_count = 0;
        // ElectricSpear.fluxSpeeds / fluxTimers
        float flux_speed[4] = {};
        float flux_timer[4] = {};
    };

    // DataPearl.AbstractDataPearl.DataPearlType, the ones with their own
    // colours (DataPearl.UniquePearlMainColor / UniquePearlHighLightColor)
    enum class pearl_type : uint8_t {
        misc,
        misc2,
        cc, ds, gw, hi, lf_bottom, lf_west, sb_filtration, sh, si_top, si_west, sl_bridge, sl_moon, sb_ravine, su, uw, sl_chimney, red_stomach,
        // Downpour
        si_chat3, si_chat4, si_chat5, spearmasterpearl, su_filt, dm, lc, lc_second, oe, ms, rm, rivulet_stomach, cl, vs, broadcast_misc,
        // the Watcher
        waua, wora, drone, abstract, text_audio_talkshow, text_stardust, text_notionofself, text_kitesday, text_secret, text_contempt,
        audio_jam1, audio_jam2, audio_jam3, audio_jam4, audio_groove, audio_voicewind1, audio_voicewind2,
        count
    };

    // DataPearl: the type's colours (ApplyPalette) and the glimmer (Update)
    struct pearl_state {
        pearl_type type = pearl_type::misc;
        bool has_highlight = false;
        // AbstractDataPearl.hidden: lies in debris until something moves it
        bool hidden = false;
        rw::vec3 highlight{1.f, 1.f, 1.f};
        rw::vec2 hidden_pos{0.f, 0.f};
        float glimmer = 0.f;
        float last_glimmer = 0.f;
        float glimmer_prog = 1.f;
        float glimmer_speed = 0.f;
        int32_t glimmer_wait = 0;
    };

    // DeerGraphics.Antlers.Part positions, flattened: the part it belongs to,
    // its position and radius, and Part.indPos for both sides
    struct antler_point {
        rw::vec3 pos{0.f, 0.f, 0.f};
        rw::vec3 ind[2] = {{0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}};
        float rad = 0.f;
        uint8_t part = 0;
    };

    // ScavengerOutpost.PearlString: one pearl, its connection length and
    // whether the connection still holds (the item's slot and EntityID
    // number, so a reused slot isn't mistaken for it)
    struct string_pearl {
        uint16_t item = 0xFFFF;
        int32_t seed = 0;
        float rad = 0.f;
        uint8_t active = 0;
    };
}
