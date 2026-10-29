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
./build/rwcpp --fuzz 600         # a creature of each kind in every room, slugcats with random input
                                 # in 128 rooms at a time (the whole world ticking), then every room exit pipe
./build/rwcpp --bench 200        # ms per tick of the whole world, one thread and every thread
./build/rwcpp --audio-check      # load the sound effects, loops and ambient clips, decode whole songs, report
./build/rwcpp --audio-dump a.wav --room su_a22 --ticks 400   # mix what is heard into a WAV, headless
```

The game data is read straight from the installed game. Roots are layered:
pass several `--root`s and earlier ones take priority (mergedmods over base).
`RW_ROOT` can also point at `StreamingAssets`.

Controls: arrows/WASD move, Z/Space jump, X/J throw, Left Shift/L pick up
(hold down while pressing it to drop), push into a pipe entrance to travel it,
stand still in a gate's middle zone to cycle it, K spawns a centipede at the
cursor (Shift+K a big one, Ctrl+K removes them all), V spawns a dropwig on the
ceiling above the cursor (Ctrl+V removes them all), Q spawns a white squidcada at
the cursor (Shift+Q a dark one, Ctrl+Q removes them all), Y spawns a lizard at the
cursor, the next breed each time (Shift+Y the same breed again, Ctrl+Y removes them
all), H spawns a daddy long legs at the cursor (Shift+H the next variant: daddy,
brother, mother, hunter; Ctrl+H removes them all), R respawn, left click teleports
the slugcat, PageUp/PageDown change room, Home/End change region, C toggles
between the vanilla camera positions and a follow camera, F1 HUD, F2 camera
outlines, G collision geometry overlay, B day/night, F5 adds a co-op player (driven by the gamepad), P pause, N single
step, T slow motion, 1-9/0 Survivor, Monk, Hunter, Gourmand, Artificer,
Rivulet, Spearmaster, Saint, Sofanthiel, Watcher (0 again toggles the
Watcher's Void Weaver decorations), E holds the Watcher's ability
(camouflage, levitation; gamepad north button), `[`/`]` lower/raise the
Watcher's ripple level (any other slugcat's karma), F6 toggles a rain storm
(a sandstorm over sand), I spawns the next kind of item at the cursor, M plays a song of the
region (again for another one; the game itself has long silent stretches),
Ctrl+M stops the music. Spearmaster pulls a spear out of its tail by holding
pick up with a free hand; Hunter stows a held spear on its back (or takes it
back) by holding pick up. A gamepad (left stick, d-pad,
south button jump, west throw, east pick up) also drives player one when
playing alone.

## Storage

Every game object is stored in a DataContainer (`src/dcon_generated.txt`),
owned by `sys::game_state::world`. Sizes are fixed bounds chosen as powers of
two a bit above vanilla's needs (4096 rooms, 2^23 tiles, 255 players, ~4000
of each creature for the whole world...), see the comment at the top of the
definitions. Ownership (room of a camera, chunks of an object, body of a
player...) is modelled with relationships. Flags written while the rooms
update in parallel are bytes, not bitfields (a bitfield shares its byte with
seven other objects).

Every creature, whatever its kind, is one `creature` object with a `type`
tag (`src/creature_types.hpp`). What at least half of the five kinds have is
a property of its own, stored SoA like everything else: the generator,
stun, death and health (every kind), the AI's behaviour and destination
(every kind), the current utility (four), the tracked slugcat, whether it is
seen, for how long it hasn't been and where it was last (every kind; the
centipede has no visual contact flag), the grasps on a slugcat (centipede,
dropwig, lizard), the forbidden idle spot (centipede, squidcada, lizard) and
the graphics module's generator (every kind); the body is a `physical_object`
(`creature_body`). What only one kind has is its `payload`, a union of one
struct per kind of at most 64 bytes (one cache line, `static_assert`ed:
centipede 64, dropwig 60, squidcada 56, lizard 52, daddy 40 bytes): its
hottest state and the index of its kind's data object (`centipede_data`,
`dropbug_data`, `cicada_data`, `lizard_data`, `daddy_data`), which holds the
rest SoA: AI modules, Lizard.cs' movement, the tongue, the graphics
modules with the lizard cosmetics. An index in the payload rather than a
relationship: one load, no link arrays, and the data object is made and
deleted with its creature anyway.

Repeated parts of a creature with a state of their own are `creature_part`s
(owned through `part_ownership`, in the order they were made): a daddy long
legs' tentacles and tube graphics. What parts share is SoA (kind, number,
stun, health: `DaddyTentacle.stun`, `DaddyState.tentacleHealth`), the rest a
`part_payload` (28 bytes: a tube's connections, chunks and deadness) pointing at the
kind's data object with the vector pools (a tentacle's tiles, grab path and
chunks; a tube's segments, bumps and parts). Body chunks stay `body_chunk`s,
shared with the slugcats and the items and moved by the one
`physics::update_object`. The graphics modules' limbs and generic body parts
(`player::hand_state`, `player::body_part`, updated by `body_parts.hpp` for
every kind) stay in the kinds' graphics state: they are cosmetic, only
update where a camera looks, and vanilla interleaves them with each kind's
own code, so no shared pass could take them.

## Simulation

There is no AbstractRoom/AbstractCreature layer: every room of every loaded
region (2074 with the Watcher) is populated at startup and simulated every
tick, viewed or not, with full physics and AI for every creature in it.
`simulation::tick` (`src/simulation.cpp`):

- **Startup**: `simulation::populate` does what `Room.Loaded` does on a
  room's first visit (placed and random items, scavenger outposts and
  treasuries, story items, fruit, rot, warp points, the sand height map) for
  every room, in room order. Every room has its own `UnityEngine.Random`
  (`room.random`), seeded from the world seed (`game_state::random`) and the
  room's name, so a room rolls the same whatever else happens. Objects are
  created serially; the sand maps are built in parallel.
- **Serial, before**: players in a shortcut travel their vessel (coming out
  moves them, what they hold and the squidcada they carry to another room),
  the viewed rooms are worked out (the players' rooms and
  `game_state::camera_rooms`), creatures in a room that just came into view
  get their graphics modules reset (`reset_positions`: the parts back on the
  body, the look kept), and `rooms::build_index` buckets every player,
  creature, item and spit by room.
- **Parallel**: one TBB task per room runs its players, creatures (kind by
  kind: every creature's Update and graphics module, then its collisions
  with the slugcats and its kin, `creature::update_room`), outposts, items, rot, water, sand and mud, ambient particles
  and gate, in that order. A room's update reads and writes only its own
  objects: every loop over creatures is `rooms::for_each_*` over the room's
  bucket, and what holds a slugcat is looked for in the slugcat's room. Each
  room has its own caches and buffers (`sys::room_runtime`): path fields,
  terrain proximity, ceiling tiles, water surface, sand, and this tick's
  noises, sounds, loop requests, music, particles and ripple rings.
- **Transactions**: whatever would change the data container's structure
  (creating or deleting objects, a lizard's spit, Spearmaster's needle, a
  rot meal, a bomb gone off, a centipede eaten by the rot) is queued on the
  room with `game_state::defer` and applied after the parallel phase, room
  by room in order, each room's in the order queued; outside the parallel
  phase `defer` runs at once. Grasps of items are plain properties of the
  player and the item, so picking up and throwing need no transaction.
- **Serial, after**: the rooms' sounds, loop requests, music, particles and
  rings are merged in room order; the Watcher's warps (which move everything they pull
  to another room and drive the screen wide sequence) update, every warp
  room in order; then the particles.

The outcome doesn't depend on the thread count or the scheduling: rooms
don't see each other while they update, every random roll comes from the
room's or the object's own generator, and the transactions apply in a fixed
order. `--selftest` runs a scenario on one thread and on every thread and
compares a hash of every body chunk; `--bench` does the same for the whole
world.

Graphics modules (centipede, dropwig, squidcada, lizard with its
cosmetics, daddy long legs) only update in viewed rooms, like vanilla where a
creature off screen has no `graphicsModule`. Where the simulation reads the
graphics module, the off screen path is vanilla's `graphicsModule == null`
one: `Lizard.LegsGripping` is 2 and `NoGripCounter` 0, the Caramel doesn't
rear up for its lunge, tongue segments, head flicker and antennae twitches
aren't touched (their random rolls are still made). Particles, sounds and
music only come from viewed rooms. The slugcat's room is always viewed, so
its graphics (which hold the hands items hang from) always run. The water
surface runs in every room with water.

`--bench N` puts a creature of each kind in every room and times N ticks of
the whole world on one thread and on every thread. On a 6 core Xeon
(Skylake, KVM):

| world | creatures | 1 thread | 6 threads | ticks/s |
| --- | --- | --- | --- | --- |
| vanilla, 760 rooms | none (no den spawns yet) | 3.8 ms | 0.87 ms | 1150 |
| vanilla, 760 rooms | 3525, one of each per room | 53 ms | 9.5 ms | 106 |
| + Watcher, 2074 rooms | none | 12.8 ms | 2.6 ms | 381 |
| + Watcher, 2074 rooms | 9705, one of each per room | 175 ms | 29 ms | 34 |

With a creature of each kind in all 2074 rooms about half the time goes to
the daddy long legs' tentacles (`Tentacle`/`DaddyTentacle`: terrain
collisions, grab path searches, ray traces), and a quarter to building path
fields (~60 a tick, mostly squidcadas and daddies changing destination); the
rest is the other creatures, the rot and the water. Rooms scale to about 6x
on 6 cores; the serial phases take ~0.25 ms a tick.

### Adding a creature kind

1. `src/creature_types.hpp`: an entry in `creature::type` (its place is its
   update order in a room) and a payload struct of at most 64 bytes in the
   `payload` union (`static_assert`ed), with a `data` index if it has a data
   object.
2. `src/dcon_generated.txt`: optionally a `<kind>_data` object for what
   doesn't fit the payload, its accessor in `creature.hpp`, its making and
   deleting in `creature::make`/`erase`; `creature_part` kinds for repeated
   parts with a state of their own.
3. The kind's code (`create` through `creature::make`, `destroy` through
   `creature::erase`, `update`, `violence`, `release_player`, the collision
   hooks) uses the shared properties and `creature::stun_countdown`,
   `consious`, `body`, `chunk`, `player_of` where vanilla's Creature does.
4. A case in the switches of `src/creature.cpp` (create, destroy, describe,
   update, collides/collide_player/collide_kin, came_into_view, violence,
   release_player) and in render.cpp's `build_creatures`.

Weapon hits, explosions, flare bombs, warps, pipes, the rot's prey, the
slugcat letting go, the fuzz and the bench then take it as they come.

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
| `src/items.*` | rocks (`Rock`/`Weapon`): vanilla spawning (`Room.Loaded`, done for every room at startup), flight/bounces/wall hits, `Player.GrabUpdate`/`ThrowObject` |
| `src/item_graphics.*` | `Rock`, `Spear` (and explosive/electric spears, Spearmaster's needles), `DataPearl` and `ScavengerOutpost` `DrawSprites` with the game's elements |
| `src/outposts.*` | the scavengers' loot without scavengers: `ScavengerOutpost` (toll spears, `DeerGraphics.Antlers`, `PearlString`s), `ScavengerTreasury`, story `MultiplayerItem`s |
| `src/gates.*` | `RegionGate` airlock state machine and door tiles |
| `src/audio.*`, `src/sound_types.hpp` | SDL3 audio: sound effects from `sounds.txt`, loops kept up by the simulation's per tick requests, the rooms' ambient sounds, music triggers and songs; the simulation only queues requests on the game state |
| `src/unity_bundle.*`, `src/fsb5.*` | reading the game's audio out of its Unity asset bundles (UnityFS, LZ4) and FMOD FSB5 banks (PCM16, Vorbis rebuilt as Ogg) |
| `src/simulation.*` | one tick of the whole world: every room in parallel (TBB), the deferred transactions, vessels and warps; startup population of every room |
| `src/rooms.hpp` | the objects of a room: per tick buckets of players, creatures (by room and kind), items and spit by room, for the parallel room updates |
| `src/creature.*`, `src/creature_types.hpp` | the `creature` object's type tag and payloads, `creature_part`'s; what every kind shares (body, chunks, Creature.Update's stun countdown, consciousness, collisions with the slugcats and kin, the projectile trace, explosions and flashes, letting go) and the dispatch to the kinds |
| `src/centipede.*` | `Centipede`/`CentipedeAI`: crawling on any surface, grabbing, shocking, utility AI |
| `src/centipede_graphics.*` | `CentipedeGraphics`: gripping legs, feelers, body roll, `DrawSprites` with the game's centipede elements |
| `src/dropbug.*` | `DropBug`/`DropBugAI`: the dropwig sitting in the ceiling, dropping on prey, pouncing, biting and carrying, its `CeilingSitModule` |
| `src/dropbug_graphics.*` | `DropBugGraphics`: long legs and knees, mandibles, tail pincers, antennae, ceiling camouflage, `DrawSprites` with the game's elements |
| `src/cicada.*` | `Cicada`/`CicadaAI`: the squidcada flying, sitting on the terrain, circling, bump attacks and tentacle clinging, being carried by the slugcat and lifting it |
| `src/cicada_graphics.*` | `CicadaGraphics`: wings, tentacles, the body's turn and the eyes, `DrawSprites` with the game's elements |
| `src/lizard.*`, `src/lizard_types.hpp` | `Lizard`: every breed's body, legs-driven running, wall climbing, swimming, the jaws (bite, grab, shake, carry), `LizardTongue`, `LizardJumpModule`, `LizardSpit`, `Violence` with the head shield |
| `src/lizard_ai.*` | `LizardAI`: utility comparison, hunting, idling, lurking, investigating sounds, the spit tracker, the Yellow pack |
| `src/lizard_breeds.cpp` | `LizardBreeds.BreedTemplate`: every breed's `LizardBreedParams`, template and tongue constants |
| `src/lizard_graphics.*` | `LizardGraphics`: gripping limbs, tail, head and tongue, head colour flashes, camouflage, `DrawSprites` with the game's elements |
| `src/body_parts.*`, `src/sprites.hpp` | `BodyPart`/`GenericBodyPart`/`Limb` helpers and the Futile sprite/mesh commands the graphics ports hand the renderer |
| `src/warps.*` | Watcher `WarpPoint`s: trigger, pull, transfer, one way/ripple/limited use warps, dynamic targets, the transition timer, ripple rings |
| `src/water.*` | the water surface: a damped wave height field over x and depth, wall reflections, swells, splashes and wakes from bodies |
| `src/particles.*` | cosmetic particles: rain, ceiling drips, splash droplets, hit sparks and flashes, landing dust, bubbles |
| `src/terrain.*` | the Watcher's sand (a deformable height map from the TerrainHandles, sandstorms) and mud pits |
| `src/explosions.*` | bombs, explosive spears, the Artificer: blast, damage, flash; FlareBomb's blinding flash |
| `src/daddy.*`, `src/daddy_types.hpp` | `DaddyLongLegs`/`DaddyAI`: the four templates, support from gripping tentacles, path following, eating, `Violence` on body and tentacles, a one slugcat `Tracker` and the `NoiseTracker` |
| `src/daddy_tentacle.cpp` | `Tentacle`/`DaddyTentacle`: tile segments, grab paths (`FindTentaclePath`), pulling, growing, chunks on `Rope`s, climbing, hunting, grabbing, examining noises |
| `src/daddy_graphics.*` | `DaddyGraphics`: leg graphics (`RopeGraphic`), dead legs, danglers, bumps, the eyes, the Hunter's dummy, `DrawSprites` with the game's elements |
| `src/rot.*` | `DaddyCorruption`: rot bulbs, little legs, tendrils, climbable rot vines (`Rope`), eating creatures; `ClimbableVinesSystem` |
| `src/pathing.*` | on demand distance fields, cached per room |
| `src/render.*` | OpenGL 3.3 (GLEW, glm) renderer in an SDL3 window: rooms through anthiel's level shader, slugcat sprites, geometry overlay, VGA font HUD |
| `src/selftest.*` | scripted movement scenarios, the thread count determinism check, the fuzzer and the bench |
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

The dropwig (`--dropbug x,y`, V) follows `DropBug`/`DropBugAI`: three
chunks that cling to floors, walls and ceilings (its StaticWorld tile
resistances), and a `CeilingSitModule` that scores the room's ceiling tiles
like vanilla (floor altitude, fall risk tile, narrow spaces, the camera,
exit distances for a ground creature) and takes the best one. There it folds
up into the ceiling, its colour fading into the palette's camouflage, and
waits: a slugcat walking under it makes it drop (one passing nearby makes
it twitch, one holding a spear close by or a spear thrown at it makes it
let go early), biting or grabbing on landing, then it hunts for a while,
charging up and pouncing from the ground, before looking for another
spot. A grabbed slugcat is worried and slashed loose, a dead one carried off
(to a room exit: there are no dens). Rocks, spears and explosions hurt it
like `DropBug.Violence`. Not ported: items it carries as bait, threats
(nothing here scares it), rain, dens and leaving rooms, being grabbed,
the AImap visibility terms, mud, Safari control.

The squidcada (`--cicada x,y[,b]`, Q, Shift+Q for the dark one) follows
`Cicada`/`CicadaAI`, white CicadaA and dark CicadaB alike: two chunks
that fly with a fluttering lift tied to their stamina, follow paths through
the air (`CicadaPather`'s preference for open space far from the terrain,
as an `open_air` cost in the distance fields) and land next to the
terrain or on poles to sit, wings folding back. Idle ones pick sit spots
around the room and now and then two of them dance in a circle
(`CicadaAI.CircleGroup`). A slugcat coming close is antagonized like
vanilla's dynamic relationship works it out (closer is worse, worse still
when it carries a squidcada; a timid one fears an armed slugcat and flees
instead): it swoops to spots around it, charges bump attacks that knock
it and stun both, and clings to it with its tentacles, flapping away with
it. The slugcat picks one up with both hands (Grab, it can't while it charges
or keeps its distance) and the squidcada flaps to hold it up:
`LiftPlayerPower` from its stamina, a boost when jumping, a slow glide
when falling, tiring while it lifts and resting while it doesn't; pick up
lets go of it, throw tosses it, and it panics away when let go. It goes
through pipes with the slugcat. Rocks, spears and explosions hurt it
through `Creature.Violence`. Not ported: the looping wing sounds (no
looping sound emitters here, only the one shot sounds), the `CicadaWing`
shader (the wings are their sprites tinted and faded), dens, swarm rooms
and rain, flies and leeches to hunt and carry, the social memory beyond
this cycle, `CreatureLooker`'s interest scores (it looks at the slugcat
it is busy with or sees), screen shake, mud, Safari control.

Lizards (`--lizard x,y[,breed]`, Y) follow `Lizard`/`LizardAI` for every
breed: Pink, Green, Blue, Yellow, White, Red, Black, Salamander, Cyan and
Downpour's and the Watcher's Caramel (`spit`), Eel, Zoop, Train, Blizzard,
Basilisk, Indigo and Peach, with their `LizardBreedParams` transcribed from
`LizardBreeds.BreedTemplate` and their TileTypeResistances as the breed's
path field movement (swimmers path through water at their
`waterPathingResistance`). Three chunks run where the legs grip: the
`LizardLimb`s step from grip to grip (`Limb.FindGrip`, walls and ceilings
for the wall climbers) and `GetFrameSpeed` moves the body only as fast as
they hold on, by terrain speed, with the body wiggling and pulling harder
when stuck (`desperationSmoother`), turning around on floors
(`LizardTurn`), dropping, grabbing poles and swimming. `LizardAI` compares
its prey, injury, noise and lurk utilities: it spots the slugcat with the
breed's vision cone (the Black lizard is blind and hears instead), hunts
it, lunges, shoots its tongue (Blue, White, Salamander, Cyan, Zoop,
Indigo, Peach) and bites; a bite kills at the breed's chance, otherwise
the slugcat is grabbed, shaken and carried to the den (a room exit, where
it is eaten), and a slugcat held in a lizard's jaws for 60 ticks is done
for (`Player.dangerGrasp`). Idle lizards wander between idle spots by the
breed's taste, lurkers (White, Salamander, Eel, Peach) wait for prey, Red
and Caramel spit (`LizardSpit`) at prey out of reach, Cyan finds and makes
jumps (`LizardJumpModule`, chain jumps included), White camouflages into
whatever is behind it, the Yellow pack shares what each one sees. Rocks
turn a lizard around, the head shield deflects what hits it head on (a
hit in the open mouth hurts more), spears and explosions hurt it, and
attacking it lowers its like of the slugcat. `LizardGraphics` draws the
game's own elements in vanilla's order: body circles and mesh, the tail
mesh (coloured towards the tip), the legs (`LizardArm_NN`, picked by
reach and depth), the jaw, teeth, head and eyes in four angles (the
Watcher's heads come from its mod atlas in the install), the tongue, the
head's colour flashing with the blink, the lunge and the voice. Not
ported: the `LizardCosmetics` (pass 2, see `lizard_graphics.cpp`), lizards
fighting and signalling each other (aggression, submission), friendship
and taming, rain, dens and shortcuts, the threats a lizard would flee,
reach-over-gap paths, the voices' articulation (a sample plays once, and
only Pink, Green and Blue have voices without Remix), the Blizzard's beam,
the Basilisk's poison field, the Indigo's mud, burrowing in the Watcher's
sand, rot, `LizardBubble`s, light sources and the CyanLizardSmoke,
drowning, AImap visibility in the idle and lurk scores, Safari control.

Daddy long legs (`--daddy x,y[,variant]`, H) follow `DaddyLongLegs`,
`DaddyAI`, `DaddyTentacle` and `Tentacle` in the four templates that run
that code: the Daddy, the Brother, Downpour's Mother (`TerrorLongLegs`,
superSizeMe) and the Hunter long legs (`HunterDaddy`, HDmode). The
constructor's rolls (chunk count and sizes, tentacle count and lengths,
the graphics layout) come from `rw::unity_random` in vanilla order. The
body is 4 to 15 chunks tied in every pair; each tentacle is a list of
room tiles grown, pulled straight and searched around terrain with
Tentacle's own tile path code (`FindTentaclePath`), with chunks following
them on Ropes that wrap the tile corners and stick to the terrain. The
gripping tentacles carry the body along a distance field from
`pathing.hpp` (its TileTypeResistances: every open tile), the rest reach
for grips ahead (`Climb`), examine noises and hunt. A daddy is blind: its
tentacles' touch tells the Tracker of the slugcat, a hunting tentacle that
brushes it with its tip grabs it and drags it in, and touching the body
starts eating it (it dies half way and is gone at the end). It hears
noises (`NoiseTracker`), its eyes turning to them and to what the
tentacles feel. Spears and rocks hit the body or a tentacle (stunning and
hurting that tentacle), explosions hurt it a lot more. `DaddyGraphics`
draws the leg graphics, dead legs and danglers with their bumps and little
eyes, the eye bulbs with their slits and the Hunter's slugcat dummy with
the game's elements. Not ported: shortcuts (squeezing and the
BigCreatureShortCutSqueeze), rain and dens, other creatures as prey or
being touched (slugcats only), daddies eating each other, the timeline
rule that makes a Brother a Daddy before Artificer's time, `stuckPos`,
Safari control, `DaddyBubble`s and `DaddyRipple`, the digestion sound loop
(and the react sounds, which have no entry in `sounds.txt`), pulling the
eaten slugcat's graphics in, the Tracker's ghosts (it guesses the slugcat
is where it last felt it), the JaggedCircle and RippleBasic shaders (a
jagged disc and plain triangles).

Paths aren't pre-baked like vanilla's AImaps and per creature A*: a
creature asks `pathing::field_to(room, goal tile)` and gets a distance field
(reverse Dijkstra with vanilla's connection costs: steps, open diagonals,
drops) kept in the room's `room_runtime::path_fields`, a cache outside the
data container (never saved). Fields are shared by every creature of the room
heading to the same tile, reused until the room's `geometry_version`
changes (gate doors, spear beams), recycled least recently used (32 a room)
and dropped after 400 ticks unused; following one is a neighbour lookup.
Fields are built with Dial's algorithm (a ring of cost buckets: the costs are
small integers) over the room's cached tile accessibility.

### Rot

`DaddyCorruption` from the rooms' `PlacedObjects:` (Corruption,
PassiveCorruption with its `CustomRotData`, CorruptionTube,
CorruptionDarkness): when the room is populated (at startup) the air tiles touching a wall
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

### Decals, particles and water

CustomDecals (`decals/<name>.png`) are stretched over their quads as grid
meshes and painted onto the level only where its depth lies within the
decal's range (never on open sky), split between the passes behind and in
front of the sprites like the level (`assets/shaders/decal.*`), with the
opacity of their corners blended across them and broken up by the decal's
noise. Their per corner smudge isn't used yet.

Particles are cosmetic and kept outside the data container: rain streaks
(rooms' RainIntensity, or everywhere during an F6 storm) that burst on
terrain and ripple water, drops forming under ceilings (CeilingDrips),
splash droplets, sparks and flashes where weapons hit walls and creatures
and where centipedes shock, dust where the slugcat lands hard or pushes off,
and bubbles from swimmers (more when out of breath) and from anything
plunging in. Hits and landings come from the sounds played that tick.

The water is our own instead of vanilla's: the surface is a height field
over x and six rows of depth with a damped wave equation, so splashes and
wakes spread, interfere and bounce off the walls cutting through the surface,
with three swells of different lengths and directions (from the room's wave
settings) crossing over it. It is drawn smooth (Catmull-Rom between nodes) as
a 3D band going back from the front edge, shaded by the slopes with the
palette's surface and shine colours, over a body that refracts and tints the
scene behind it (`assets/shaders/water.frag`): the light fades with depth,
and the current's shimmer and caustics are domain warped hashed noise, so
they never repeat and still play down at the floor of the deepest pools.
Bodies entering or leaving the water push the surface and splash; moving
along it leaves a wake. It is cosmetic: creatures still float on the flat
water level. Rain and drops stop at the surface, rippling it. The
Artificer can't drown: a third of its breath gone, it explodes under water
(throwing everything close away, hurting centipedes).

### Items, plants, sand and mud

Besides rocks and spears there are explosive spears (go off where they hit
or stick), electric spears (three charges of shock for what they hit),
scavenger bombs (explode on impact), flare bombs (a blinding flash that
stuns centipedes), lily pucks (cutting discs), and food and pearls that are
tossed rather than thrown: mushrooms and glowing slime mold growing where
placed, dangle fruit and rotten fruit hanging from the ceiling on a stalk
until picked, data pearls in their type's colours with their highlight
and glimmer (`DataPearl`). The placed ones come from the room settings,
`ReliableSpear`s and story `MultiplayerItem`s too; a few random spears are
explosive or electric. Scavenger outposts stand without their scavengers:
the pole, the antlers (`DeerGraphics.Antlers` rolled from the outpost's
skull seed) and skulls, strings of misc pearls hanging from the antler
tips, and 13 to 24 toll spears lying around; treasuries spread spears
(three in four explosive), bombs and pearls on the floor around them
(lanterns aren't ported). The toll, the guards and theft aren't modelled.
Rocks, spears (the explosive spear's rag swings, the electric spear's
zappers flicker) and pearls are drawn with the game's sprites
(`Rock`/`Spear`/`ExplosiveSpear`/`ElectricSpear`/`DataPearl` DrawSprites);
the rag's and the pole's JaggedSquare edges, the antlers' OutPostAntler
paint (approximated from its formula with hashed noise) and the pearls'
FlatLightBehindTerrain glow (a plain soft disc) are approximations. Corn plants (SeedCob, eaten DeadSeedCob, RottenSeedCob) stand at
rest. Dangle fruit (with its stalk), corn plants and centipedes are drawn
like vanilla (`DangleFruit`, `SeedCob` and `CentipedeGraphics` DrawSprites)
with the game's own sprites, read from its Unity data at startup; the corn
plants' layout is rolled with UnityEngine.Random's own generator and seed.
Without the Unity data they, like the other items, fall back to simple
shapes of our own. Eating isn't ported.

The Watcher's sand is a 1D height map built from the room's TerrainHandles
(a spline through them) at startup, kept for the session only. Bodies stand on it; their weight slowly presses marks into
it and pushes the sand aside, and slopes too steep for sand slump. A
sandstorm (F6 in a room with sand) blows sand through the air and slowly
works the dunes back into smooth sine dunes. Mud pits drag and slowly
swallow whatever walks in. Rot in the Outer Rim and the rot regions is the
Watcher's purple.

### Audio

The game's loose sound and music files are empty placeholders; the real ones
live in `StreamingAssets/AssetBundles` (`loadedsoundeffects`,
`loadedsoundeffects_ambient`, `music_songs`). Those are read directly from the
install, nothing is bundled: UnityFS with LZ4 blocks, the AudioClips of the
serialized file, and their FMOD FSB5 data, PCM16 or Vorbis whose stripped
setup header is restored from vgmstream's table and rewrapped as Ogg for
stb_vorbis. Sound effects follow `soundeffects/sounds.txt` (volume and pitch
ranges, the trigger's own `vol`/`pitch`/`rangeFac` flags, PLAYALL,
silentChance, `_1`.. `_n` variants), panned and attenuated around the camera
(fading out past the screen's edges, `rangeFac` stretching the fade). The
slugcat's jumps, rolls, landings and fall impacts, throws, pick ups, weapons
hitting walls, shortcut ticks, gate doors and the creatures play them. Music
follows the rooms' `Triggers:` (MusicEvent/StopMusicEvent spot and region
triggers with fire chance, delay, priority, fade in/out and room count).
Threat music and the procedural region ambience aren't ported.

**Loops** (`DynamicSoundLoop`, `ChunkDynamicSoundLoop`, `StaticSoundLoop`,
`RectangularDynamicSoundLoop`, `DisembodiedDynamicSoundLoop` and the
emitters with `requireActiveUpkeep`): an object that wants a loop asks for it
every tick with `game_state::play_loop` (a `sound::loop_request`: an owner key
from `sound::loop_key`, the sound, the room, the position, a rect for
rectangular ones or a pan for disembodied ones, volume, pitch, and
StaticSoundLoop's `fadeOutOnDestroyFrames`). Like `play_sound`, only viewed
rooms make requests, into the room's `room_runtime::loops` while the rooms
update in parallel; they are merged into `game_state::loops` in room order
after the parallel phase (warps add theirs serially after), which holds just
this tick's requests, and `game_state::loop_tick` counts the ticks. Requests
never feed back into the simulation (the world hash is unchanged); the little
state the loops need (a squidcada's current loop, the daddy's digestion
volume, a gate's, a warp point's, the Watcher's camouflage sound) is cosmetic.
The audio side (`audio::upkeep`, `audio::fade`, no device needed, tested by
`--selftest`) reconciles its voices once per new tick: matched by owner, a
loop follows its request; one whose owner didn't ask again (turned it off,
volume 0, died, was destroyed, left the room) fades out, over its
`fadeOutOnDestroyFrames` or a 60 ms declick (vanilla cuts it); another sound
for the same owner starts a new voice with a fresh sample variant; loops of
a room the camera left stop at once. Paused, nothing changes. A voice is an
SDL stream per sample (all of them for PLAYALL) fed the sample over and over
0.1 s ahead with the pan baked in, its gain and frequency ratio set every
frame; 24 at most.

What loops: the squidcada's wing hum and its bump attack prepare/charge
variants (CicadaGraphics), the centipede's crawl and its charge's crackle
(CentipedeGraphics; Centiwing_Fly_LOOP comes with centiwings), the
lizards' lunge and belly drag (LizardGraphics; neither is in sounds.txt, so
silent unless a mod adds them, like vanilla), the daddy long legs' and
brother's digestion (DaddyGraphics), the slugcat's slide loops (belly slide,
roll, skid, wall slide, sliding down a pole or through a corridor;
stunned, sample-less, silences them like vanilla), the Watcher's camouflage
activating/deactivating loop, rocks skidding on the ground (the rock and
spear through the air loops are commented out in sounds.txt), the water
surface (a rect along the default water level), gates' background
machinery (Gate_Water/Electric_Working_Background_LOOP from the mode, electric
gates from `gates/egates.txt`), and warp points' idle, suck in and ripple
hint loops. Left for later, with what they need: the other RegionGateGraphics
loops (steam, waterfall, clamps, screws: the gate graphics), waterfalls
(`WaterFall` objects), the rain cycle's rain/rumble/flood loops (a hook in
`particles::ambient`: RoomRain's sounds follow the cycle's intensity), the
Watcher's dynamic warp and ripplesickness loops, Mushroom_Trip_LOOP
(mushrooms), shelter machinery (shelter doors). The dropwig, the rot and
shortcut travel have no loops in vanilla.

**Room ambience** (`AmbientSoundPlayer`): the `AmbientSounds:` of every
room's settings (OMNI, DIR and SPOT with volume, pitch, position, radius and
taper; the room's own plus its template's it doesn't overwrite,
`RoomSettings.InheritAmbientSounds`) are `ambient_sound` objects. The audio
side plays the camera room's: entering a room keeps the omnidirectional
players playing the same sample (taking the new volume and pitch, like
`VirtualMicrophone.NewRoom`) and stops the others; clips stream from a mod's
`loadedsoundeffects/ambient/` or the `loadedsoundeffects_ambient` bundle,
looping. Omnidirectional ones play centred, directional ones panned by their
direction, spot ones by `InverseLerp(taperRad, rad, distance)` from the
listener (the slugcat) and panned across the taper radius. Not done: the
doppler pitch, the underwater low pass, the ripple space ambience, and
vanilla's volume curve (`Volume exponent: 1.8`, every sound here is linear).

Missing samples, clips or no audio device degrade silently: the requests are
still made and dropped. `--audio-check` reports which sounds and loops have
samples and decodes every room's ambient clip; `--audio-dump out.wav` (with
`--room`, `--ticks`, `--script`, the spawn options) runs headless, mixes what
the camera would hear into a WAV and reports its loudness every half second
and every loop's start, stop and peak gain.

### Shortcuts, items, gates

Pushing into a pipe entrance pulls the slugcat in like
`Creature.enteringShortCut`, then a vessel travels the traced shortcut path at
vanilla's speed (one tile every third tick) but continuously, so it is drawn
as a soft circle gliding along the pipe and lighting nearby pipe tiles instead
of vanilla's tile by tile blink. In-room pipes spit the slugcat out at the
other end; room exits follow `world_<region>.txt` connections (merged across
region files, gates included) to the destination's matching exit. Every one
of the 1414 connected room exits is exercised by `--fuzz`.

Rocks and spears spawn like `Room.Loaded` does on a room's first visit, for every room at startup: placed
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
anchors, plus the 4 segment tail mesh. Other ported `DrawSprites` (centipedes,
dropwigs, squidcadas, lizards, daddy long legs, dangle fruit, corn plants) use the game's own Futile atlases (`rainWorld`,
`rainworldmsc`, `uiSprites`, `uispritesmsc`) out of `resources.assets` in the
game's `RainWorld_Data` (`--unity <dir>`) and the Watcher's
`mods/watcher/atlases/rainworldwatcher` from the install; nothing of it is bundled. The level is drawn in two passes split
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
The grime noise drifts very slowly. Not done yet: skyboxes and other room
effects, `RegionGateGraphics`.

## License

GPLv3 (see LICENSE), like anthiel which this is based on. The slugcat sprite
atlas is packed from the DressMySlugcat template sprites
(github.com/MatheusVigaro/DressMySlugcat), which are derived from Rain World.
