#include "creature.hpp"
#include "player.hpp"

// CreatureTemplate.relationships for every pair of ported templates, the
// slugcat included: StaticWorld.InitStaticWorldRelationships, ...MSC and
// ...Watcher run in order with EstablishRelationship's spread to the
// descendants (the lizard breeds of LizardTemplate, the centipede, leech,
// squidcada and long legs variants of theirs), from each template's
// constructor default (Ignores; the centipedes, leeches, long legs and the
// dropwig Eats 1; the squidcadas Uncomfortable), with the Cyan lizard's Eats
// cut to 0.3 at the end. Downpour (MSC, DLCShared) and the Watcher are on.
// Transcribed by a script that replays StaticWorld.cs; row: the one who
// relates, column: the one related to (the script reproduces the earlier
// hand made TubeWorm row and column: LizardTemplate Eats it at 0.025,
// DaddyLongLegs Ignores it; it Ignores everything). The rows and columns
// after it, the same way: BigNeedleWorm with its SmallNeedleWorm young,
// TentaclePlant with PoleMimic (its descendant), JetFish, Scavenger, Deer,
// MirosBird.

namespace creature {
    namespace {
        constexpr rel i = rel::ignores, e = rel::eats, a = rel::attacks, f = rel::afraid, u = rel::uncomfortable, r = rel::agressive_rival,
                      s = rel::stay_out_of_way, n = rel::antagonizes, p = rel::plays_with, d = rel::social_dependent, k = rel::pack;
        constexpr relationship table[tmpl_count][tmpl_count] = {
        // Slugcat
        {{i,0.f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,0.3f}, {i,0.3f}, {f,0.3f}, {f,1.f}, {f,0.3f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,0.5f}, {i,0.f}, {i,0.f}, {i,0.f}, {f,1.f}, {f,1.f}, {i,0.f}, {i,0.f}, {i,0.f}, {f,0.8f}},
        // PinkLizard
        {{d,0.5f}, {r,0.4f}, {r,0.15f}, {r,0.2f}, {r,0.1f}, {r,0.25f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.15f}, {r,0.1f}, {e,0.3f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {e,0.8f}, {e,0.8f}, {e,0.8f}, {f,0.9f}, {e,0.8f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,0.05f}, {e,0.05f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {e,0.2f}, {e,0.025f}, {e,0.25f}, {e,0.3f}, {f,0.2f}, {f,0.2f}, {e,0.1f}, {e,0.8f}, {i,0.f}, {i,0.f}},
        // GreenLizard
        {{d,0.5f}, {r,0.2f}, {r,0.8f}, {e,0.25f}, {r,0.1f}, {r,0.05f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {e,0.3f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {e,0.3f}, {e,0.8f}, {e,0.8f}, {e,0.8f}, {f,0.5f}, {e,0.8f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,0.05f}, {e,0.05f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {e,0.2f}, {e,0.025f}, {e,0.25f}, {e,0.3f}, {f,0.2f}, {f,0.2f}, {e,0.1f}, {e,0.8f}, {i,0.f}, {i,0.f}},
        // BlueLizard
        {{d,0.5f}, {r,0.3f}, {f,0.25f}, {r,0.45f}, {r,0.1f}, {f,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {f,0.2f}, {f,0.25f}, {f,0.25f}, {e,0.5f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {e,0.8f}, {e,0.8f}, {e,0.8f}, {f,0.9f}, {e,0.8f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,0.7f}, {e,0.7f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {e,0.2f}, {e,0.025f}, {e,0.25f}, {e,0.3f}, {f,0.2f}, {f,0.2f}, {e,0.1f}, {e,0.8f}, {i,0.f}, {i,0.f}},
        // YellowLizard
        {{d,0.5f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {k,0.2f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {e,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.45f}, {e,0.8f}, {e,0.8f}, {e,0.8f}, {f,0.9f}, {e,0.8f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,0.05f}, {e,0.05f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {e,0.2f}, {e,0.025f}, {e,0.25f}, {e,0.3f}, {f,0.2f}, {f,0.2f}, {e,0.1f}, {e,0.8f}, {i,0.f}, {i,0.f}},
        // WhiteLizard
        {{d,0.5f}, {r,0.15f}, {r,0.05f}, {r,0.35f}, {r,0.1f}, {r,0.25f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.05f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {e,0.8f}, {e,0.8f}, {e,0.8f}, {f,0.9f}, {e,0.8f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,0.7f}, {e,0.7f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {e,0.2f}, {e,0.025f}, {e,0.25f}, {e,0.3f}, {f,0.2f}, {f,0.2f}, {e,0.1f}, {e,0.8f}, {i,0.f}, {i,0.f}},
        // RedLizard
        {{d,0.5f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {a,1.f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {e,0.3f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {e,0.8f}, {e,0.8f}, {e,0.8f}, {f,0.9f}, {e,0.8f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,0.05f}, {e,0.05f}, {f,0.2f}, {f,0.2f}, {f,0.2f}, {f,0.2f}, {e,0.2f}, {e,0.025f}, {e,0.25f}, {e,0.3f}, {f,0.2f}, {f,0.2f}, {e,0.1f}, {e,0.8f}, {i,0.f}, {a,0.4f}},
        // BlackLizard
        {{d,0.5f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.05f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {e,0.8f}, {e,0.8f}, {e,0.8f}, {f,0.9f}, {e,0.8f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,0.05f}, {e,0.05f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {e,0.2f}, {e,0.025f}, {e,0.25f}, {e,0.3f}, {f,0.2f}, {f,0.2f}, {e,0.1f}, {e,0.8f}, {i,0.f}, {i,0.f}},
        // Salamander
        {{d,0.5f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {e,0.8f}, {e,0.8f}, {e,0.8f}, {f,0.9f}, {e,0.8f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,0.05f}, {e,0.05f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {e,0.2f}, {e,0.025f}, {e,0.25f}, {e,0.3f}, {f,0.2f}, {f,0.2f}, {e,0.5f}, {e,0.8f}, {i,0.f}, {i,0.f}},
        // CyanLizard
        {{d,0.5f}, {r,0.1f}, {r,0.1f}, {e,0.045f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.075f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {e,0.24f}, {e,0.24f}, {e,0.24f}, {f,0.9f}, {e,0.24f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,0.015f}, {e,0.015f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {e,0.06f}, {e,0.0075f}, {e,0.075f}, {e,0.09f}, {f,0.2f}, {f,0.2f}, {e,0.03f}, {e,0.24f}, {i,0.f}, {i,0.f}},
        // SpitLizard
        {{d,0.5f}, {r,0.2f}, {r,0.8f}, {e,0.25f}, {r,0.1f}, {r,0.05f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {e,0.25f}, {e,0.3f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {e,0.3f}, {e,0.8f}, {e,0.8f}, {e,0.8f}, {f,0.5f}, {e,0.8f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,0.05f}, {e,0.05f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {e,0.2f}, {e,0.025f}, {e,0.25f}, {e,0.3f}, {f,0.2f}, {f,0.2f}, {e,0.1f}, {e,0.8f}, {i,0.f}, {i,0.f}},
        // EelLizard
        {{d,0.5f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {e,0.8f}, {e,0.8f}, {e,0.8f}, {f,0.9f}, {e,0.8f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,0.05f}, {e,0.05f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {e,0.2f}, {e,0.025f}, {e,0.25f}, {e,0.3f}, {f,0.2f}, {f,0.2f}, {e,0.1f}, {e,0.8f}, {i,0.f}, {i,0.f}},
        // ZoopLizard
        {{d,0.5f}, {f,0.35f}, {f,0.5f}, {r,0.65f}, {r,0.1f}, {r,0.1f}, {f,1.f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {f,1.f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {e,0.8f}, {e,0.8f}, {e,0.8f}, {f,0.9f}, {e,0.8f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,0.05f}, {e,0.05f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {e,0.2f}, {e,0.025f}, {e,0.25f}, {e,0.3f}, {f,0.2f}, {f,0.2f}, {e,0.1f}, {e,0.8f}, {i,0.f}, {i,0.f}},
        // TrainLizard
        {{d,0.5f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {e,0.8f}, {e,0.8f}, {e,0.8f}, {f,0.9f}, {e,0.8f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,0.05f}, {e,0.05f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {e,0.2f}, {e,0.025f}, {e,0.25f}, {e,0.3f}, {f,0.2f}, {f,0.2f}, {e,0.1f}, {e,0.8f}, {i,0.f}, {i,0.f}},
        // BlizzardLizard
        {{d,0.5f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {e,0.8f}, {e,0.8f}, {e,0.8f}, {f,0.9f}, {e,0.8f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,0.05f}, {e,0.05f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {e,0.2f}, {e,0.025f}, {e,0.25f}, {e,0.3f}, {f,0.2f}, {f,0.2f}, {e,0.1f}, {e,0.8f}, {i,0.f}, {i,0.f}},
        // BasiliskLizard
        {{d,0.5f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {e,0.8f}, {e,0.8f}, {e,0.8f}, {f,0.9f}, {e,0.8f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,0.05f}, {e,0.05f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {e,0.2f}, {e,0.025f}, {e,0.25f}, {e,0.3f}, {f,0.2f}, {f,0.2f}, {e,0.1f}, {e,0.8f}, {i,0.f}, {i,0.f}},
        // IndigoLizard
        {{d,0.5f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {e,0.3f}, {e,0.8f}, {e,0.8f}, {e,0.8f}, {f,0.9f}, {e,0.8f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,0.05f}, {e,0.05f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {e,0.2f}, {e,0.025f}, {e,0.25f}, {e,0.3f}, {f,0.2f}, {f,0.2f}, {e,0.1f}, {e,0.8f}, {i,0.f}, {i,0.f}},
        // PeachLizard
        {{d,0.5f}, {r,0.1f}, {f,0.5f}, {r,0.1f}, {r,0.45f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {f,0.7f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {r,0.1f}, {f,0.55f}, {k,0.2f}, {e,0.8f}, {e,0.8f}, {e,0.8f}, {f,0.9f}, {e,0.8f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,0.05f}, {e,0.05f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {e,0.2f}, {e,0.025f}, {e,0.25f}, {e,0.3f}, {f,0.2f}, {f,0.2f}, {e,0.1f}, {e,0.8f}, {i,0.f}, {i,0.f}},
        // Centipede
        {{e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {f,0.5f}, {i,0.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}},
        // SmallCentipede
        {{e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {f,0.5f}, {i,0.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}},
        // Centiwing
        {{e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {f,0.5f}, {i,0.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}},
        // RedCentipede
        {{e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {f,0.5f}, {i,0.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}},
        // AquaCenti
        {{e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {f,0.5f}, {i,0.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}},
        // Leech
        {{e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {f,0.3f}, {e,1.f}, {e,1.f}, {f,0.3f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,0.2f}, {e,0.2f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {e,0.4f}, {e,1.f}, {e,0.15f}, {e,0.15f}, {i,0.f}, {i,0.f}, {f,0.6f}, {e,1.f}, {e,1.f}, {e,1.f}},
        // SeaLeech
        {{e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {f,0.3f}, {e,1.f}, {e,1.f}, {f,0.3f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,0.2f}, {e,0.2f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {e,0.4f}, {e,1.f}, {e,0.15f}, {e,0.15f}, {i,0.f}, {i,0.f}, {f,0.6f}, {e,1.f}, {e,1.f}, {e,1.f}},
        // JungleLeech
        {{e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {f,0.3f}, {e,1.f}, {e,1.f}, {f,0.3f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,0.2f}, {e,0.2f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {e,0.4f}, {e,1.f}, {e,0.15f}, {e,0.15f}, {i,0.f}, {i,0.f}, {f,0.6f}, {e,1.f}, {e,1.f}, {e,1.f}},
        // CicadaA
        {{d,1.f}, {f,0.3f}, {n,0.1f}, {f,0.7f}, {f,0.3f}, {f,0.7f}, {f,0.3f}, {f,0.3f}, {f,0.3f}, {f,0.3f}, {n,0.1f}, {f,0.3f}, {f,0.3f}, {f,0.3f}, {f,0.3f}, {f,0.3f}, {f,0.3f}, {f,0.3f}, {f,0.15f}, {e,0.15f}, {f,0.35f}, {f,0.8f}, {f,0.15f}, {e,0.4f}, {e,0.4f}, {e,0.4f}, {p,1.f}, {p,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,0.5f}, {u,0.f}, {f,0.15f}, {e,0.1f}, {f,0.5f}, {f,0.5f}, {r,0.5f}, {u,0.f}, {u,0.f}, {u,0.f}},
        // CicadaB
        {{d,1.f}, {f,0.3f}, {n,0.1f}, {f,0.7f}, {f,0.3f}, {f,0.7f}, {f,0.3f}, {f,0.3f}, {f,0.3f}, {f,0.3f}, {n,0.1f}, {f,0.3f}, {f,0.3f}, {f,0.3f}, {f,0.3f}, {f,0.3f}, {f,0.3f}, {f,0.3f}, {f,0.15f}, {e,0.15f}, {f,0.35f}, {f,0.8f}, {f,0.15f}, {e,0.2f}, {e,0.2f}, {e,0.2f}, {p,1.f}, {p,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,0.5f}, {u,0.f}, {f,0.15f}, {e,0.1f}, {f,0.5f}, {f,0.5f}, {r,0.5f}, {u,0.f}, {u,0.f}, {u,0.f}},
        // DaddyLongLegs
        {{e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,1.f}, {e,1.f}, {i,0.f}, {e,1.f}, {i,0.f}, {i,0.f}, {e,0.1f}, {i,0.f}, {e,1.f}, {e,0.15f}, {i,0.f}, {i,0.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}},
        // BrotherLongLegs
        {{e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,1.f}, {e,1.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,0.1f}, {i,0.f}, {e,1.f}, {e,0.15f}, {i,0.f}, {i,0.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}},
        // TerrorLongLegs
        {{e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,0.1f}, {i,0.f}, {e,1.f}, {e,0.15f}, {i,0.f}, {i,0.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}},
        // HunterDaddy
        {{e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,1.f}, {e,1.f}, {i,0.f}, {e,1.f}, {i,0.f}, {i,0.f}, {e,0.1f}, {i,0.f}, {e,1.f}, {e,0.15f}, {i,0.f}, {i,0.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}},
        // DropBug
        {{e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {f,0.4f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,0.1f}, {e,1.f}, {f,0.8f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {f,1.f}, {i,0.25f}, {e,1.f}, {e,1.f}, {e,0.1f}, {f,0.6f}, {f,0.3f}, {e,1.f}, {e,1.f}, {e,1.f}, {f,0.9f}},
        // TubeWorm
        {{i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}},
        // BigNeedleWorm
        {{u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {f,0.8f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {f,0.9f}, {f,0.9f}, {f,0.9f}, {f,0.9f}, {u,1.f}, {u,1.f}, {a,0.9f}, {a,0.9f}, {f,0.3f}, {f,0.3f}, {u,1.f}, {u,1.f}, {u,1.f}, {f,0.9f}},
        // SmallNeedleWorm
        {{u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {f,0.8f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {u,1.f}, {f,0.9f}, {f,0.9f}, {f,0.9f}, {f,0.9f}, {u,1.f}, {u,1.f}, {a,0.9f}, {a,0.9f}, {f,0.3f}, {f,0.3f}, {u,1.f}, {u,1.f}, {u,1.f}, {f,0.9f}},
        // TentaclePlant
        {{e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,1.f}, {e,1.f}, {f,0.6f}, {f,0.6f}, {f,0.6f}, {f,0.6f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {i,0.f}, {i,0.f}, {e,1.f}, {e,1.f}, {f,0.2f}, {f,0.8f}},
        // PoleMimic
        {{e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,1.f}, {e,1.f}, {f,0.6f}, {f,0.6f}, {f,0.6f}, {f,0.6f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {i,0.f}, {i,0.f}, {e,1.f}, {e,1.f}, {f,0.2f}, {f,0.8f}},
        // JetFish
        {{d,1.f}, {f,0.2f}, {f,0.2f}, {f,0.2f}, {f,0.2f}, {f,0.2f}, {f,0.2f}, {f,0.2f}, {f,0.5f}, {f,0.2f}, {f,0.2f}, {f,0.2f}, {f,0.2f}, {f,0.2f}, {f,0.2f}, {f,0.2f}, {f,0.2f}, {f,0.2f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,0.2f}, {e,0.2f}, {e,0.2f}, {e,0.1f}, {e,0.1f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {f,0.2f}, {i,0.f}, {i,0.f}, {i,0.f}, {f,0.2f}, {f,0.2f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}},
        // Scavenger
        {{d,1.f}, {f,0.5f}, {f,0.5f}, {f,0.5f}, {f,0.5f}, {f,0.5f}, {f,0.5f}, {f,0.5f}, {f,0.5f}, {f,0.5f}, {f,0.5f}, {f,0.5f}, {f,0.5f}, {f,0.5f}, {f,0.5f}, {f,0.5f}, {f,0.5f}, {f,0.5f}, {f,0.4f}, {i,0.f}, {f,0.4f}, {f,0.95f}, {f,0.4f}, {i,0.1f}, {i,0.1f}, {i,0.1f}, {i,0.1f}, {i,0.1f}, {f,1.f}, {f,0.8f}, {f,1.f}, {f,0.8f}, {f,0.55f}, {i,0.1f}, {u,0.5f}, {i,0.f}, {f,0.65f}, {f,0.3f}, {i,0.1f}, {k,0.5f}, {i,0.1f}, {f,0.7f}},
        // Deer
        {{i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}, {i,0.f}},
        // MirosBird
        {{e,0.6f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,0.8f}, {e,0.8f}, {e,0.8f}, {f,0.7f}, {e,0.8f}, {i,0.f}, {i,0.f}, {i,0.f}, {e,1.f}, {e,1.f}, {f,0.7f}, {f,0.7f}, {f,0.7f}, {f,0.7f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {e,1.f}, {n,0.1f}},
        };
        static_assert(sizeof(table) / sizeof(table[0]) == tmpl_count);

        // CreatureTemplate.bodySize, smallCreature
        constexpr float body_sizes[tmpl_count] = {
            1.f,
            2.f, 3.f, 2.f, 2.f, 2.f, 3.f, 2.f, 2.f, 2.f, 1.f, 4.f, 2.f, 3.f, 3.f, 3.f, 3.f, 3.f,
            1.f, 0.2f, 1.2f, 8.5f, 1.f,
            0.1f, 0.15f, 0.05f,
            0.6f, 0.6f,
            5.5f, 2.5f, 3.f, 2.f,
            1.2f,
            0.25f,
            0.7f, 0.15f,
            2.f, 1.4f,
            0.8f,
            1.2f,
            10.f,
            7.f,
        };
        static_assert(sizeof(body_sizes) / sizeof(body_sizes[0]) == tmpl_count);

        constexpr char const* names[tmpl_count] = {
            "Slugcat",
            "PinkLizard", "GreenLizard", "BlueLizard", "YellowLizard", "WhiteLizard", "RedLizard", "BlackLizard", "Salamander",
            "CyanLizard", "SpitLizard", "EelLizard", "ZoopLizard", "TrainLizard", "BlizzardLizard", "BasiliskLizard", "IndigoLizard", "PeachLizard",
            "Centipede", "SmallCentipede", "Centiwing", "RedCentipede", "AquaCenti",
            "Leech", "SeaLeech", "JungleLeech",
            "CicadaA", "CicadaB",
            "DaddyLongLegs", "BrotherLongLegs", "TerrorLongLegs", "HunterDaddy",
            "DropBug",
            "TubeWorm",
            "BigNeedleWorm", "SmallNeedleWorm",
            "TentaclePlant", "PoleMimic",
            "JetFish",
            "Scavenger",
            "Deer",
            "MirosBird",
        };
    }

    relationship static_relationship(tmpl a, tmpl b) noexcept {
        return table[size_t(a)][size_t(b)];
    }

    float body_size(tmpl t) noexcept {
        return body_sizes[size_t(t)];
    }

    bool small_creature(tmpl t) noexcept {
        return t == tmpl::leech || t == tmpl::sea_leech || t == tmpl::jungle_leech || t == tmpl::small_needle_worm;
    }

    char const* to_string(tmpl t) noexcept {
        return t < tmpl::count ? names[size_t(t)] : "?";
    }

    tmpl template_of(sys::game_state const& state, dcon::creature_id c) noexcept {
        auto const& pl = state.world.creature_get_payload(c);
        switch (state.world.creature_get_type(c)) {
        case type::slugcat: return tmpl::slugcat;
        case type::centipede: return tmpl(uint32_t(tmpl::centipede) + uint32_t(pl.centipede.variant));
        case type::dropbug: return tmpl::drop_bug;
        case type::cicada: return pl.cicada.gender ? tmpl::cicada_a : tmpl::cicada_b;
        case type::lizard: return tmpl(uint32_t(tmpl::pink_lizard) + uint32_t(pl.lizard.breed));
        case type::daddy: return tmpl(uint32_t(tmpl::daddy_long_legs) + uint32_t(pl.daddy.variant));
        case type::leech: return tmpl(uint32_t(tmpl::leech) + uint32_t(pl.leech.variant));
        case type::tube_worm: return tmpl::tube_worm;
        case type::needle_worm: return pl.needle_worm.variant == needle_worm::variant::small ? tmpl::small_needle_worm : tmpl::big_needle_worm;
        case type::pole_mimic: return tmpl::pole_mimic;
        case type::tentacle_plant: return tmpl::tentacle_plant;
        case type::jetfish: return tmpl::jet_fish;
        case type::scavenger: return tmpl::scavenger;
        case type::deer: return tmpl::deer;
        case type::miros: return tmpl::miros_bird;
        case type::count: break;
        }
        return tmpl::slugcat;
    }

    float attraction_value(attraction a) noexcept {
        switch (a) {
        case attraction::forbidden: return 0.f;
        case attraction::avoid: return 0.25f;
        case attraction::like: return 0.75f;
        case attraction::stay: return 1.f;
        case attraction::neutral: break;
        }
        return 0.5f;
    }
}
