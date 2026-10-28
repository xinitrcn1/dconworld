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
```

The game data is read straight from the installed game. Roots are layered:
pass several `--root`s and earlier ones take priority (mergedmods over base).
`RW_ROOT` can also point at `StreamingAssets`.

Controls: arrows/WASD move, Z/K/Space jump, X/J throw, Left Shift/L pick up
(hold down while pressing it to drop), push into a pipe entrance to travel it,
stand still in a gate's middle zone to cycle it, R respawn, left click teleports
the slugcat, PageUp/PageDown change room, Home/End change region, C toggles
between the vanilla camera positions and a follow camera, F1 HUD, F2 camera
outlines, G collision geometry overlay, B day/night, F5 adds a co-op player (driven by the gamepad), P pause, N single
step, T slow motion, 1-9/0 Survivor, Monk, Hunter, Gourmand, Artificer,
Rivulet, Spearmaster, Saint, Sofanthiel, Watcher. A gamepad (left stick, d-pad,
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
| `deps/` | FetchContent scripts for DataContainer, TBB, glm, GLEW and stb |
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
| `src/simulation.*` | one tick of players, vessels, items and gates of the occupied rooms |
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

Not ported yet: creatures and object to object collision, zero gravity,
climbable vines, Remix (MMF) options, the other Downpour/Watcher slugcat
abilities, sand terrain (`TerrainManager`), and the `modify/` mod merge
system (needed for Downpour/Watcher regions).

### Shortcuts, items, gates

Pushing into a pipe entrance pulls the slugcat in like
`Creature.enteringShortCut`, then a vessel travels the traced shortcut path at
vanilla's speed (one tile every third tick) but continuously, so it is drawn
as a soft circle gliding along the pipe and lighting nearby pipe tiles instead
of vanilla's tile by tile blink. In-room pipes spit the slugcat out at the
other end; room exits follow `world_<region>.txt` connections (merged across
region files, gates included) to the destination's matching exit. Every one
of the 1414 connected room exits is exercised by `--fuzz`.

Rocks spawn on a room's first visit like `Room.Loaded`: placed rocks at 75%,
then `RandomItemDensity` random rocks on tiles with three solid tiles below
(not in shelters or gates; spear rolls are made but spears aren't ported).
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
foreground plants occlude the slugcat. Not done yet: arms/hands, skyboxes and
other room effects, vanilla water, `RegionGateGraphics`.

## License

GPLv3 (see LICENSE), like anthiel which this is based on. The slugcat sprite
atlas is packed from the DressMySlugcat template sprites
(github.com/MatheusVigaro/DressMySlugcat), which are derived from Rain World.
