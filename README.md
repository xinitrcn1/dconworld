# rwcpp

RainWorld rewritten in C++; aimed at demostrating that a 'slow' game like RainWorld can be fast if coupled with SoA and DoD technologies.

Heavily based off https://codeberg.org/Esther1024/anthiel

## Building

Requires CMake >= 3.28, a C++20 compiler, OpenGL and SDL3 (system packages:
`SDL3-devel`, the Mesa GL headers and `libX11-devel`). DataContainer, TBB, glm,
GLEW and stb are fetched by CMake on the first configure (`deps/`, like anthiel).

```sh
cmake -S . -B build -G Ninja
cmake --build build -j2
```

The project is developed on a weak machine, so the build is capped at two
parallel compile jobs (a Ninja job pool in `CMakeLists.txt`; with Makefiles
pass `-j2` yourself) and optimises at `-O2` (`RelWithDebInfo` by default).

## Running

```sh
./build/rwcpp                    # finds the Steam install, starts in SU_A22
./build/rwcpp --room sl_a02      # any room by name
./build/rwcpp --root <StreamingAssets> [--root <more>...]
./build/rwcpp --slugcat rivulet  # any slugcat, Downpour and Watcher included
./build/rwcpp --selftest         # scripted movement/item/gate tests (no game data needed)
./build/rwcpp --fuzz 600         # random input soak test in every room, then every room exit pipe
./build/rwcpp --audio-check      # load the sound effects, decode whole songs, report
```

The game data is read straight from the installed game. Roots are layered:
pass several `--root`s and earlier ones take priority (mergedmods over base).
`RW_ROOT` can also point at `StreamingAssets`.

Controls: arrows/WASD move, Z/Space jump, X/J throw, Left Shift/L pick up
(hold down while pressing it to drop), push into a pipe entrance to travel it,
stand still in a gate's middle zone to cycle it, K spawns a centipede at the
cursor (Shift+K a big one, Ctrl+K removes them all), R respawn, left click teleports
the slugcat, PageUp/PageDown change room, Home/End change region, C toggles
between the vanilla camera positions and a follow camera, F1 HUD, F2 camera
outlines, G collision geometry overlay, B day/night, F5 adds a co-op player (driven by the gamepad), P pause, N single
step, T slow motion, 1-9/0 Survivor, Monk, Hunter, Gourmand, Artificer,
Rivulet, Spearmaster, Saint, Sofanthiel, Watcher (0 again toggles the
Watcher's Void Weaver decorations), E holds the Watcher's ability
(camouflage, levitation; gamepad north button), `[`/`]` lower/raise the
Watcher's ripple level (any other slugcat's karma), M plays a song of the
region (again for another one; the game itself has long silent stretches),
Ctrl+M stops the music. Spearmaster pulls a spear out of its tail by holding
pick up with a free hand; Hunter stows a held spear on its back (or takes it
back) by holding pick up. A gamepad (left stick, d-pad,
south button jump, west throw, east pick up) also drives player one when
playing alone.

## Storage

Every game object is stored in a DataContainer (`src/dcon_generated.txt`),
owned by `sys::game_state::world`. Sizes are fixed bounds chosen as powers of
two a bit above vanilla's needs (2048 rooms, 2^23 tiles, 8 players...), see
the comment at the top of the definitions. Ownership (room of a camera,
chunks of an object, body of a player...) is modelled with relationships.

## Layout

| file | what |
| --- | --- |
| `src/gamestate.hpp` | `sys::game_state`: the `dcon::data_container world` plus non-SoA globals (constants, strings, lookup maps, filesystem) |
| `src/dcon_generated.txt` | DataContainer definitions for every object (regions, rooms, tiles, cameras, exits, shortcuts, nodes, physical objects, body chunks, connections, players) with their size bounds |
| `deps/` | FetchContent scripts for DataContainer, TBB, glm, GLEW, stb (images, stb_vorbis), dr_libs (dr_mp3, for mod songs) and vgmstream's FMOD Vorbis setup header table (one pinned, hash checked file) |
| `assets/` | our own assets, copied next to the binary: anthiel's VGA font, the room shaders (`shaders/`, from anthiel) and the slugcat sprite atlas (`slugcat.png` + `slugcat.txt`) |
| `tools/pack_dms.py` | packs DressMySlugcat template atlases into `assets/slugcat.*` (strips the guide markers, transcribes the frames to plain text) |
| `src/player_graphics.*` | simplified `PlayerGraphics` port: head/legs/tail body parts and vanilla sprite selection |
| `src/layered_fs.*` | case insensitive, layered, read only view of the game files (RomFS); files are mmap'd |
| `ParserGenerator/`, `src/parser_defs.txt` | build time generated parsers for `key: value` files (region `properties.txt`, room `_settings.txt` and templates) |
| `src/parsers.*` | tokenizer and value parsers used by the generated code |
| `src/world.*` | room file parsing (in parallel with TBB), loading into the data container, `room_view` tile helpers |
| `src/physics.*` | `BodyChunk` and `BodyChunkConnection` ports over the data container |
| `src/player.*` | slugcat movement (`Player.cs` port); players are data container objects (co-op ready) |
| `src/shortcuts.*` | shortcut travel: suck-in, the vessel moving along the pipe path, spit out, room to room transfers through `world_<region>.txt` connections |
| `src/items.*` | rocks (`Rock`/`Weapon`): vanilla spawning on a room's first visit, flight/bounces/wall hits, `Player.GrabUpdate`/`ThrowObject` |
| `src/gates.*` | `RegionGate` airlock state machine and door tiles |
| `src/audio.*`, `src/sound_types.hpp` | SDL3 audio: sound effects from `sounds.txt`, music triggers and songs; the simulation only queues requests on the game state |
| `src/unity_bundle.*`, `src/fsb5.*` | reading the game's audio out of its Unity asset bundles (UnityFS, LZ4) and FMOD FSB5 banks (PCM16, Vorbis rebuilt as Ogg) |
| `src/simulation.*` | one tick of players, vessels, creatures, items and gates of the occupied rooms |
| `src/centipede.*` | `Centipede`/`CentipedeAI`: crawling on any surface, grabbing, shocking, utility AI |
| `src/warps.*` | Watcher `WarpPoint`s: trigger, pull, transfer, one way/ripple/limited use warps, dynamic targets, the transition timer, ripple rings |
| `src/rot.*` | `DaddyCorruption`: rot bulbs, little legs, tendrils, climbable rot vines (`Rope`), eating creatures; `ClimbableVinesSystem` |
| `src/pathing.*` | on demand, shared distance fields stored in the data container |
| `src/render.*` | OpenGL 3.3 (GLEW, glm) renderer in an SDL3 window: rooms through anthiel's level shader, slugcat sprites, geometry overlay, VGA font HUD |
| `src/selftest.*` | scripted movement scenarios and the fuzzer |
| `og/` | decompiled game source used as the reference (not tracked) |

## Status

### Room geometry (`Room.LoadFromDataString`)

Complete for the level files: `RoomPreprocessor.VersionFix`, size, water level
and "water in front", light angle, camera positions (with the vanilla Y flip),
border type, placed rocks/spears, the room node connectivity map (only for
`World.preProcessingGeneration` 20 maps, like `WorldLoader`; stale ones are
flagged for regeneration), and every tile attribute (beams, shortcut
paths/exits/dens/whack-a-mole/transport, wall behind, hives, waterfalls,
garbage holes, worm grass). Shortcuts are traced like `ShortcutMapper` to get
their type, length and destination node, and `Room.exitAndDenIndex` is built.
The base64 AI heat maps (line 11) are left in the file to be decoded on demand
by a future AI preprocessor.

All 760 vanilla rooms load with no errors in ~0.6s (warm cache).

### Slugcat movement

`Player.Update`/`MovementUpdate`/`UpdateBodyMode`/`UpdateAnimation`, `Jump`,
`WallJump`, `TerrainImpact` (rolls, fall stun and death), `LungUpdate` and the
helpers (`DirectIntoHoles`, `GrabVerticalPole`, `GravitateToOpening`...) are
ported for every slugcat's stats and colours (Rivulet's movement branches
included): crawling, standing, running and skidding,
jumps/boosted jumps/super launch, wall slides and wall jumps, ledge grabs,
poles (vertical, horizontal, pull ups, beam tips, pole hopping, pole skips),
corridors (crawl, boosts, turns, drops onto poles), rolls, belly slides,
whiplash flips, rocket jumps, surface and deep swimming with breath and
drowning. The underlying `BodyChunk` physics (tile, floor and slope
collision, water, room bounds) and chunk connections are ported faithfully,
including the vanilla quirks.

Climbable vines (`ClimbableVinesSystem`, the `VineGrab` animation) are
ported for the rot vines: grabbing one while holding up or falling through
it, climbing and swinging along it, switching to a crossing vine, jumping
off, and the hands gripping it.

Not ported yet: creatures and object to object collision, zero gravity
(also on vines), the plant vines and coral, Remix (MMF) options, the other Downpour/Watcher slugcat
abilities, sand terrain (`TerrainManager`), and the `modify/` mod merge
system (needed for Downpour/Watcher regions).

### Creatures

The centipede is the first creature: a chain of body chunks (7 to 17 by
size) held by push connections and ropes, crawling along floors, walls,
ceilings and poles with either end leading, turning around when its tail is
closer to the goal. Its ends grab the slugcat on contact and once the other
end reaches it too, the shock kills a lighter slugcat or stuns a heavier one
(big centipedes hunt, small ones flee). Rocks and spears hurt and stun it
like `Centipede.Violence` (shell breaking and spears lodging aren't
ported). The AI follows `CentipedeAI`: idle wandering by idle score, fleeing
and hunting from a simplified tracker (sight and contact).

Paths aren't pre-baked like vanilla's AImaps and per creature A*: a
creature asks `pathing::field_to(room, goal tile)` and gets a distance field
(reverse Dijkstra with vanilla's connection costs: steps, open diagonals,
drops) kept in `game_state::path_fields`, a cache outside the data container (never saved). Fields are shared by every creature
heading to the same tile, reused until the room's `geometry_version`
changes (gate doors, spear beams) and recycled least recently used; following
one is a neighbour lookup.

### Rot

`DaddyCorruption` from the rooms' `PlacedObjects:` (Corruption,
PassiveCorruption with its `CustomRotData`, CorruptionTube,
CorruptionDarkness): on a room's first visit the air tiles touching a wall
inside the rot areas grow bulbs, laid out from a seed like vanilla so a room
always grows the same rot. Bulbs wobble on their stalks, look around (eyes
are the two slit meshes of `RenderSlits`), turn to hard impacts they hear
(`InGameNoise` from `PhysicalObject.TerrainImpact`) and pass touches on to
their neighbours. Hostile bulbs nibble at any creature touching them and
drag it towards the thick of the rot; once enough nibbling has added up the
creature is pulled into the wall and eaten (`EatenCreature`). Some bulbs grow
a little leg that grabs prey and pulls it in. PassiveCorruption only slows
creatures and grows hanging tendrils. CorruptionTubes are climbable vines
between two walls, simulated like `ClimbableCorruptionTube` with `Rope`
wrapping them around tile corners. Everything is drawn with plain shapes:
ragged bulbs over the level foreground (vanilla's Water container), tubes and
bumps with the region's corruption colours, a soft blot for black goo and
CorruptionDarkness. Not ported: DaddyBubbles/ripples, StuckDaddy restraints,
RotFlyPaper, the Watcher's sentient rot, slugcat filters on placed objects.

### Watcher warps

Watcher regions are added from every root's `modify/world/regions.txt`
(`[ADD]` lines only; the rest of the modify merge system isn't ported), so
`--root <StreamingAssets>/mods/watcher --root <StreamingAssets>` loads all
2074 rooms. `WarpPoint` and `DynamicWarpTarget` placed objects are read
from the room settings. A warp's tear opens as the Watcher comes close;
standing in it (still, for a faster trigger) starts the warp: gravity drops
to zero around it, the Watcher is drawn into its middle and nearby items
and creatures spiral in. Halfway through the transition the Watcher and
everything the pull held appear at the destination's warp at once (vanilla
lets them out one by one), the Watcher hovering out until the transition
clears. Only the Watcher can warp. One way warps work from their entrance
only, limited use warps close when used up, ripple warps only work from the
ripple layer (camouflaged at ripple level 5) and show as faint ripples from
the other layer; warps without a destination pick a DynamicWarpTarget of
another region the ripple level allows. Holding the ability key toggles the
camouflage after a moment (drawn as a plain fade, not vanilla's mask), with
a charge that runs out, and from ripple level 3 lifts the Watcher off the
ground. The tears and the screen wide transition are GLSL shaders of our own
(`assets/shaders/warp_*`) going for the vanilla look; the HUD shows a karma
meter, or for the Watcher a ripple meter with the camouflage charge. Not
ported: warp sounds, the Void Weaver sealing, warp fatigue, bad warps,
player made dynamic warps, ripple spawn and the ripple layer's other effects.

### Audio

The game's loose sound and music files are empty placeholders; the real ones
live in `StreamingAssets/AssetBundles` (`loadedsoundeffects`, `music_songs`).
Those are read directly: UnityFS with LZ4 blocks, the AudioClips of the
serialized file, and their FMOD FSB5 data, PCM16 or Vorbis whose stripped
setup header is restored from vgmstream's table and rewrapped as Ogg for
stb_vorbis. Sound effects follow `soundeffects/sounds.txt` (volume and pitch
ranges, PLAYALL, `_1`.. `_n` variants), panned and attenuated around the
camera; the slugcat's jumps, rolls, landings and fall impacts, throws, pick
ups, weapons hitting walls, shortcut ticks and gate doors play them (30 of
31 ported sounds; the death impact uses a UI sample outside the bundles).
Music follows the rooms' `Triggers:` (MusicEvent/StopMusicEvent spot and
region triggers with fire chance, delay, priority, fade in/out and room
count). Threat music and the procedural region ambience aren't ported.

### Shortcuts, items, gates

Pushing into a pipe entrance pulls the slugcat in like
`Creature.enteringShortCut`, then a vessel travels the traced shortcut path at
vanilla's speed (one tile every third tick) but continuously, so it is drawn
as a soft circle gliding along the pipe and lighting nearby pipe tiles instead
of vanilla's tile by tile blink. In-room pipes spit the slugcat out at the
other end; room exits follow `world_<region>.txt` connections (merged across
region files, gates included) to the destination's matching exit. Every one
of the 1414 connected room exits is exercised by `--fuzz`.

Rocks and spears spawn on a room's first visit like `Room.Loaded`: placed
rocks at 75% and spears at 60%, then `RandomItemDensity` random items on
tiles with three solid tiles below (not in shelters or gates). Spears stick
in a wall they hit straight on (from up close, or a third of the time from
further away) and lend their tiles a beam to climb; otherwise they bounce
off. Like vanilla without Remix, only Artificer can pull them back out.
Spearmaster grows needles out of its tail speckles.
Picking up, dropping (down + pick up), throwing (with recoil, flip
down-throws and the belly slide boosts), flight, floor bounces, slope
deflection and wall hits follow `Weapon`/`Rock`/`Player`. Carried rocks sit
at an approximate hand position until hands are ported.

Gates run the `RegionGate` modes (airlock closing, washing, middle door,
side doors) with vanilla door speeds, and doors turn their tiles solid while
closed. Karma locks aren't enforced (karma isn't tracked) and the doors are
drawn as plain slabs.

### Rendering

Rooms are drawn from their camera images (`<room>_<n>.png`, placed at the
vanilla camera positions) with anthiel's level shader: every palette a room
or region uses is a `palette` object in the data container and one layer of a
palette texture array; `effectcolors.png` provides effect colours A/B and
grime. Slugcats use vanilla element names (`BodyA`, `HipsA`, `HeadA0-17`,
`FaceA/B0-8`, `LegsA*`) from a DressMySlugcat style atlas with the vanilla
anchors, plus the 4 segment tail mesh. The level is drawn in two passes split
on vanilla's depth threshold: background depths first, then sprites (slugcats,
rocks, gate doors), then only the foreground depths over them, so pipes and
foreground plants occlude the slugcat. Slugcats have their hands (`SlugcatHand` limbs: gripping poles, ledges and
the floor while crawling, swimming strokes, flailing in the air, reaching
after a throw or over the shoulder for the back spear; carried items sit in
them). Slugcat specifics: Gourmand's wide
torso, Spearmaster's thin body and tail speckles (eyes shut while pulling a
needle), Artificer's mushroom scar, Rivulet's axolotl gills (sprites from the
DressMySlugcat extras sheet), and the decorated Watcher: longer tail, gold
feathers, gold eyes and a halo.
The grime noise drifts very slowly. Not done yet: arms/hands, skyboxes and
other room effects, vanilla water, `RegionGateGraphics`.

## License

GPLv3 (see LICENSE), like anthiel which this is based on. The slugcat sprite
atlas is packed from the DressMySlugcat template sprites
(github.com/MatheusVigaro/DressMySlugcat), which are derived from Rain World.
