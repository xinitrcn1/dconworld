#pragma once

// Port of the LizardCosmetics (Template and its subclasses: SpineSpikes,
// BumpHawk, the BodyScales - ShortBodyScales and the LongBodyScales
// LongShoulderScales, LongHeadScales, TailTuft, AxolotlGills -, TailFin,
// TailGeckoScales, Whiskers, Antennae, JumpRings, WingScales, BodyStripes,
// SnowAccumulation) and the Watcher's (SkinkStripes, SkinkSpeckles,
// BasiliskSlowField, BlizzardHaloShine, BlizzardBeam, BlizzardSteam,
// PeachHeadStripes, PeachBackFin): the frills, spikes, scales, whiskers and
// antennae on the lizards, rolled per lizard by LizardGraphics' constructor
// and drawn with the game's own elements (LizardScaleA/B, Circle20, ...).
// Their state is lizard::cosmetics_state, at the end of the graphics state.

#include <vector>

#include "gamestate.hpp"
#include "lizard_graphics.hpp"
#include "lizard_types.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace lizard_cosmetics {
    // the LizardGraphics constructor's AddCosmetic calls, with their random
    // rolls from the graphics module's generator (between the
    // blackSalamander roll and the blink)
    void add_cosmetics(sys::game_state& state, dcon::creature_id l);
    // Template.Reset for each cosmetic
    void reset(sys::game_state& state, dcon::creature_id l);
    // Template.Update for each cosmetic
    void update(sys::game_state& state, dcon::creature_id l, world::room_view const& room);
    // Template.DrawSprites (and ApplyPalette) of the cosmetics whose sprites
    // AddToContainer puts at `overlap`, in their order; `pal` is the
    // lizard's own palette (breed_palette), `head` the LizardHead sprite
    // (the head stripes and halo copy it), `jitter` DrawSprites' randomness
    void build(sys::game_state const& state, dcon::creature_id l, float alpha, lizard_graphics::palette const& pal, lizard::sprites_overlap overlap,
        sprites::sprite const& head, rw::rng& jitter, std::vector<sprites::draw_cmd>& out);
}
