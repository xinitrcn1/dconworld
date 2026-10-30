# NPC slugcats (SlugAI)

Slugcats without a controller, driven by a brain of our own. Vanilla's
`SlugNPCAI` is not ported and not used. This document covers what they do,
how the code is laid out, the movement model and the measurements behind it,
the tools for tracing them, and what is known not to work yet.

## What they do

- **Spawning.** At world load a share of the shelters (`--npcs <share>`,
  default 0.25; `--no-npcs` for none) each get a group of 1 to 8 slugcats,
  rolled with the shelter's own generator (`npc::spawn_world`). Each one is
  any slugcat (Survivor to the Watcher, Downpour's included; a Watcher is a
  Void Weaver half the time) in a random colour (random hue, saturation 0.25
  to 1, lightness 0.3 to 0.85; light eyes on a dark body). In game, `U`
  drops a group of three at the cursor and `Ctrl+U` removes every NPC.
- **Packs.** In a group of two or more, each slugcat joins the pack with
  a 50% chance; with at least two members the first is the leader. Members
  copy the leader's target and set off when it does (`npc::update_world`,
  serial). The pack shares what it sees, like the Yellow lizards' YellowAI:
  a member that doesn't see a threat a mate in the room sees takes it over.
  The leader waits at a pipe (up to 400 ticks) for members of the room that
  are further than 7 tiles from it. When the leader dies, the first living
  member takes over (and keeps the target).
- **Target shelter.** Each slugcat (each pack's leader) picks a random
  shelter it can reach: in its region or in any region it can get to
  through gates and Watcher warps, as far as its movement allows (see
  [Routes](#routes)). In the target it rests until its region's rain cycle
  is over (`rain::cycle::cycles` changes), or for 4800 ticks in a region
  where it never rains, then picks another one. When the rain is less than
  1600 ticks away it makes for the nearest shelter once a cycle ("hurried").
- **Fleeing.** It doesn't fight. A creature that would kill or eat a
  slugcat (that creature's own dynamic relationship to it: `go_for_kill`,
  intensity at least 0.05), in sight and within 360 px, sends it running.
  It runs into a pipe it is closer to than the threat (every 40 ticks it
  looks again), else straight away from the threat. It forgets a threat 100
  ticks after it last saw it. Fleeing through a pipe off the planned way
  ends the trip, and a new one is planned from the next room.
- **Held** by a creature it wriggles (random input); stunned, or being
  sucked into a pipe, it does nothing.

## Code

| file | what |
| --- | --- |
| `src/npc.hpp`, `src/npc.cpp` | spawning, packs, the world level routes and their search, the brain (`think`), the moves, the debug reports and options |
| `src/npc_types.hpp` | `npc::nav_state`: the brain's state in the data container (goal, move, timers, threat, failed moves...) |
| `src/pathing.cpp`, `src/pathing.hpp`, `src/pathing_types.hpp` | `movement::slugcat`: the slugcat's path fields, its jumps (`pathing::jump`, `step_kind`), `slugcat_step`, `slugcat_reach_from` |
| `src/dcon_generated.txt` | the `slugcat_npc` object; `player::npc` points at it (none for a controlled player). `player` is bounded at 4096 |
| `src/simulation.cpp` | NPCs update with their room after the controlled players, their input from `npc::think`; their vessels travel serially like the players'; only controlled players make a room "viewed" |
| `src/selftest.cpp` | the NPC scenarios, `--npc-soak`, `--npc-trip`, the measurement harness |

An NPC is an ordinary `player` (Player.cs' state) with an ordinary slugcat
`creature`, so everything else treats it as a slugcat: lizards hunt it, the
rot eats it, the rain kills it, the gates cycle for it. The warps never
start their screen-wide sequence for an NPC Watcher (`warps.cpp`'s
`player_usable` skips NPCs); an NPC is carried by `warps::carry` like a
creature. The HUD's meters and the ripple layer look at the first
controlled player.

`think` runs inside the room's parallel update and only reads the room and
static data (the routes). Everything an NPC needs from another room (its
pack leader's target) is copied serially in `update_world`. Every roll
comes from the slugcat creature's own generator, so the world comes out the
same on any thread count (the selftest's determinism run has NPCs in it).

Arriving in a room through a pipe moves the creature's trip on
(`migrate::arrived`); a warp has done that already (`placed_in_room`),
which `think` tells apart by `time_in_room` being 0.

## Routes

`npc::build_routes` works out, once at world load (every room in
parallel), which of a room's exits and warps a slugcat can walk between.
A room's *goals* are its room exits (by `exitAndDenIndex`, at most 48) and
its warps (bits 48 to 55). For each goal a slugcat path field is built, and
`slugcat_room_routes::from_exit[n]` gets the goals a slugcat coming out of
exit `n` can reach. `from_arrival[j]` does the same for where each warp
leading into the room puts it. Gate rooms count as passable both ways (the
airlock lets it through). The fields are dropped after.

A trip is a breadth first search over (room, how it got in) states
(`explore`): from the goals reachable from where the slugcat stands, through
each exit to the connected room's matching exit (shelters and gates
included, the way back has to exist), and through warps. Its first 12 hops
become the creature's `trip` (`migrate.hpp`), which `migrate::travel_tile`
follows: the pipes, the gates' middle doors, the warps. A longer way is
planned again when those run out.

Before the moves below existed, half the exits of the vanilla world led
nowhere for a slugcat (699 of 1298 exits in 510 rooms with more than one
goal). With them it is 423 (`--npc-dead-ends`). Many of the rest really
need tech the graph doesn't have (see [Not done](#not-done)), some are one
way.

## Moving within a room

### The path graph

`movement::slugcat` fields are built by their own Dijkstra
(`build_slugcat`), not the creatures' AImap rules: those treat the tile
just past a ledge as floor, which would have a slugcat walk over a two tile
gap. A slugcat's tiles are:

- **standable**: open, over solid ground, a slope or a platform, or up on a
  horizontal beam (not a beam tile itself);
- **walkable**: standable, a beam, a pipe entrance or a pipe's mouth, a
  platform tile (it climbs up through them and drops through them), water,
  a corridor (`access::corridor`), or a one tile hole in a floor (AImapper's
  floor: it runs over it).

Plain steps go to the 8 neighbours: sideways freely, diagonally between two
standable (or two water) tiles with both sides open, up and down only where
there is something to climb (a beam, a corridor, water, a pipe, a platform),
or up between two standable tiles. A sideways step onto a beam's top is only
allowed from beside it standing, or up from hanging on it. Straight drops
are allowed off any walkable tile down an open column (at most 30 tiles: a
long fall stuns).

Everything else is a **jump** (`pathing::jump`), found once per room and
geometry version by a forward search and kept sorted by the tile it lands
on (`room_runtime::jumps`):

| kind | from | to |
| --- | --- | --- |
| `drop` | ground at a ledge | down the open column beside it |
| `rise` | ground, a corridor's or a pole's top | onto the ledge on either side of the open column above, up to 3 tiles; straight up to a pole, a beam or a corridor (2–3 tiles); a hop onto a pole 1–3 tiles away (±3 up/down) |
| `leap` | the edge of solid ground, two standable tiles of run-up behind it | over the gap onto ground or a pole/beam |
| `pounce` | one tile back from the edge (the body crouches over two tiles) | over the gap onto ground or a pole/beam |
| `swing` | a vertical pole | another vertical pole 2–8 tiles across (grabbed on the way), or ground |
| `tip` | a pole's top tile (standing on its tip) | ground around it, or a pole starting 1–5 tiles over it |
| `bounce` | a pole's top tile | into a wall 1–2 tiles to one side and off it back to a pole 3–6 tiles above, in the same column or one tile out |
| `wall` | what it stands or hangs on at the bottom of a shaft 2–3 tiles wide | chained wall jumps up to what it can hold on to higher up, or out over the top |
| `pipe` | in front of a pipe within the room | in front of its other end |

Jumps need their arc clear: the body (two tiles) along a parabola rising
`height` tiles over a level jump (1 for pounces and pole jumps, 2 for leaps
and tip jumps), down to the landing, plus headroom where it leaves if it
goes up. Costs are in tenths of a tile: a step is 10 (a pole 15, water 20,
a beam's top 40), a jump roughly 20–40 plus 10 a tile. A jump at the very
end of its reach costs 40 more, so a start with some margin wins. Rises
from a beam's top cost 60 more. Wall jumps cost 300 + 30 a tile: they fail
the most and are a last resort.

### Reaches (measured)

Every reach comes from running the Player.cs port in a test room
(`--selftest --npc-measure` runs the harness in its current form), and
from experiments in the real rooms where a test room disagreed
(`--npc-exp`). Tiles are counted from where the jump leaves, `dy` up:

- **Jump up to a ledge** (standing, holding towards it, the ledge grab):
  1–3 tiles; 4 fails against the wall.
- **Running jump** from the edge after a run-up (gap = tiles of air):
  dy +3 gap ≤ 3, dy +1/+2 gap ≤ 4, dy 0 to −3 gap ≤ 5.
  Used as `leap_reach`: +3 → 4, +1/+2 → 5, ≤ 0 → 6 tiles from the edge.
- **Charge pounce** (crawl, hold jump until `superLaunchJump` is 20, let go,
  hold towards it), from a tile back from the edge: about 140–160 px flat,
  peak ≈ 23 px. Gaps: dy +2 → 2, +1 → 5, 0 → 7, −1 → 8, ≤ −2 → 9+.
  Used as `pounce_reach` from its start: +2 → 4, +1 → 7, 0 → 9, −1 → 10,
  lower → 11.
- **Off a pole's side** (facing the way, jump with the direction held,
  then up): catching another pole: 1–4 tiles across at up to +1, 5 at 0,
  6 dropping 2, 7 dropping 4, 8 dropping 7 (`swing_rise`; not from the
  pole's top tile, 6 and more across cost extra). Onto a ledge, from the
  tile held, started like a player (up and towards 2 ticks, towards 2):
  +2 → 4, +1 → 5, 0/−1 → 6, −2 to −4 → 7, −5 to −7 → 8, lower → 9
  (`pole_jump_reach`; holding only the direction 4 ticks it is a tile
  shorter level and 2 down). CC_A02's jump into its middle corridor is 8
  across and 5 down, with the ledge grab.
- **From a pole's tip** (up onto `BeamTip`, then jump): onto ground: +4 →
  1, +3 → 4, 0 to +2 → 5, lower → 6 (`tip_reach`). HI_A10's jump onto the
  outcrop is 5 over and 2 up: further than in the test room, it crawls onto
  the outcrop. Up to a pole starting `k` tiles over the tip: k ≤ 2 → 5
  across, 3–4 → 4, 5 → straight up only (HI_C11's).
- **Bounce**: off the pole into a wall beside it and off the wall back:
  with the wall beside the pole it catches a pole up to 2 tiles over the
  start; further from the wall it comes back lower. From a pole's *tip*
  into the wall one tile away and straight back it catches the pole six
  tiles above (CC_B06's move).
- **Wall jumps** zigzagging from wall to wall climb shafts 2 and 3 tiles
  wide indefinitely (36+ tiles); from 4 wide up they don't. Up a single
  wall they don't (only Rivulet's do; not modelled).

### Following the field

Each tick `go` takes the NPC's tile (its feet's, else its head's, else a
walkable neighbour; on a pole the head's, or just over it at the very top),
moving to the head's tile when that is further along. `slugcat_step` picks
the neighbour or jump with the lowest distance + edge cost, skipping the
moves that failed lately. A plain step is plain input (walking, climbing,
swimming, crawling a corridor, standing up where there is room since it
runs faster than it crawls; off a pole onto a beam beside it only with its
feet level with the beam, or it would hang). A jump starts a **move**,
input over several ticks:

- `rise`: off the wall first (jumping from a wall climb would push it
  away), stand up, then jump towards the ledge holding it, and hold up.
  From a beam's top it doesn't hold up for the first 22 ticks (it would
  catch the beam again). Holding a pole and rising straight up, it does a
  tip jump instead.
- `back_up`, `run`: two tiles back for a run-up (unless it is already
  running that way), then jump at the edge and hold it.
- `crouch`, `charge`: down on fours facing the gap with both ends on the
  ground, hold jump until `superLaunchJump` is 20, let go.
- `swing`: onto the pole first (down out of a pipe's mouth), face the way
  (it jumps off the side it faces; it only turns when it has to), jump,
  hold up for the other pole; onto ground it is a flight.
- `tip`: up onto the tip, jump.
- `bounce`: up onto the tip, jump into the wall, into it with jump let go
  (3 ticks), then jump off it the other way holding up.
- `wall`: towards the first wall (off a pole or a beam facing it first);
  under the shaft straight up holding jump, then against the first wall
  touched; against a wall in the air, jump off it towards the other; hold up
  (catching the beams on the way), except right after leaving the start.
- `flight`: in the air after a jump: hold the direction (and jump, for a
  running jump), reach up early when it aims for a pole or a beam, at tick
  15 for a ledge; a ledge grab climbs up. It lands on the ground, a pole,
  water or in a corridor; against a wall it keeps reaching.

Outside a move: hanging from a ledge it climbs up, hanging under a pole it
climbs onto it, in the air it holds its course (reaching up when the way on
is up, grabbing a pole the path wants). A pipe whose mouth is a beam under
the rock: off the beam it stands on, catch it hanging, push in along it.
Next to a pipe it pushes into it; at a gate's closed middle door it waits.

**Getting unstuck.** No progress for 150 ticks: a hop now and then. A move
that times out or ends somewhere else is *blocked* (from, to: four kept)
and the next best move is taken. Every 200 ticks without progress the
blocked moves get another go (otherwise two equally bad steps can make it
oscillate). 600 ticks without progress: the trip is dropped and planned
again; after three of those the target is dropped.

## Tracing and debugging

| option | with | what |
| --- | --- | --- |
| `--npc-trip <from> <to> [ticks]` | | one NPC (`--slugcat`, else random; `--npc-seed`) from shelter to shelter, no creatures, the rain timers frozen (`game_state::rain_frozen`). Prints each room it enters; fails when it dies, falls out of a room (the port doesn't kill for that, the game does), finds no way, gets stuck, or runs out of time |
| `--npc-soak [ticks]` | `--npcs` | the world with its NPCs: ms per tick, shelters reached, regions crossed, moves used, deaths and who killed them |
| `--npc-human-complete <from> <to>` | `--npc-human-log`, `--script` | watch the NPC; when it fails, drive it yourself from the pipe it came in by, logged, to the pipe it was heading for (below) |
| `--npc-trace <room\|*>[,every]` | trip, human-complete, selftest | input, `can_jump`/`want_to_jump`, facing, move timer, describe, the step taken, aim, goal, best distance, stuck, body mode, animation, head and feet every few ticks while in that room |
| `--npc-route <r1,r2,...>` | trip | `ok`/`NO` for each room of a route: can the way out be walked from the way in |
| `--npc-maps` | route | for each `NO` room: its poles and a map, `f` reachable from the way in, `o` can reach the way out, `B` both. The gap between `f` and `o` is the missing move |
| `--npc-room <room>` | soak | the room's poles, pipes (type, ends), map (`.` walkable) |
| `--npc-goal <bit>` | room | mark (`o`) what reaches that exit/warp |
| `--npc-box <x0,y0,x1,y1>` | room, goal | the walkable tiles there: stand/walk, pole/hbeam/pipe, distance |
| `--npc-edges <room,x0,y0,x1,y1>` | soak | the jumps leaving those tiles: kind, landing, cost, side |
| `--npc-dead-ends` | soak | the exits from which no other exit can be walked to |
| `--npc-unreachable` | soak | the route data of the first NPCs with no shelter in reach |
| `--npc-list` | soak | every NPC at the end |
| `--npc-measure` | `--selftest` | the movement measurement harness instead of the tests |
| `--npc-exp <room,x,y,mode,dir[,turn]>` | trip | a scripted jump off the pole at x climbed to y in a real room: 0 off its side, 1 from its tip, 2 into a wall and off it, 3 off, back to the wall behind and off it, 4 from the tip into a wall and back, 5 off the side into a wall and back |

### Showing it the way: `--npc-human-complete`

```sh
./build/rwcpp --root game/mergedmods --root game --npc-human-complete hi_s05 uw_s04 --slugcat survivor
```

The game's window, the trip test's world (no creatures, no other NPCs, the
rain frozen). The camera follows the NPC on its way (hold Tab to watch at
four times the speed). When it dies, falls out of a room, gets stuck or
finds no way, it is put back where it came into that room (the pipe's mouth
it came out of, or its spawn in the first shelter), its brain is removed
and the keyboard or gamepad drives it. The log says which pipe to take: the
one the planner would leave the room by, planned afresh from there. Dying
puts it back again and starts the recording over. Going into that pipe (or
reaching the room behind it, for a warp) ends the program and writes the
input to `npc_human_<from>_<to>.txt` (or `--npc-human-log <file>`):

```
# --npc-human-complete hi_s05 -> uw_s04, Survivor
# room cc_b08: in by the pipe at 20,3 (feet at ...), out by the pipe at 10,72 to cc_a15
# attempts 1, 812 ticks; the input as --script (U D L R J X P E, N nothing; then the ticks held):
R40,RJ12,N5,...
```

The last line is `--script`'s format. Given back with `--script "<line>"`
(and no window), the same run replays it headless as what the human does
and prints PASS or FAIL: whether the recorded way works, and a sequence of
inputs to study for the move the graph is missing.

The step kinds in the trace and `--npc-edges` are `step_kind`'s order:
0 walk, 1 drop, 2 rise, 3 leap, 4 pounce, 5 swing, 6 tip, 7 bounce,
8 wall, 9 pipe.

Debugging a room usually goes: `--npc-route` to find it, `--npc-maps` to
see where the two regions meet, `--npc-room --npc-goal --npc-box` for the
exact tiles, `--npc-exp` to find a move that physically works there,
measure it, add it to the graph; then `--npc-trace` the trip through it.

## The HI_S05 → UW_S04 test

Survivor, no creatures, the rain frozen:

```sh
./build/rwcpp --root game/mergedmods --root game --npc-trip hi_s05 uw_s04 --slugcat survivor
```

The route (from the user): HI_S05 → HI_A10 → HI_C11 → GATE_HI_CC → CC_A16 →
CC_A02 → CC_C08 → CC_B06 → CC_B05 → CC_B08 → CC_A15 → CC_C07 → CC_A06 →
CC_H01 → GATE_CC_UW → UW. Every room of it is connected in the graph
(`--npc-route`), and the NPC walks it on its own as far as CC_B08:

| room | what it takes |
| --- | --- |
| HI_S05 | down the shelter's shaft into the pipe in its floor |
| HI_A10 | its in-room pipe to the left, up the poles, a tip jump onto the outcrop (5 over, 2 up), up the ledges, the top-left pipe onto the upper pole, a pole jump to the exit's pole |
| HI_C11 | tip jumps between the poles' sections, then off the pole into the 2 wide shaft and up its beams (wall jumps between them), the platform at the top, the pole to the gate |
| GATE_HI_CC | waits at the middle door, the gate cycles for it |
| CC_A02 | off pole x=14 near its top into the middle corridor (8 over, 5 down, ledge grab), along it, pole to pole, a jump onto the ledge with the exit |
| CC_C08 | a straight jump up to a beam, and in along the beam into the low pipe |
| CC_B06 | a pole jump across, then from the lower pole's tip into the wall and back onto the pole above (the bounce), a jump onto the platform by the exit |
| CC_B05 | beams over a pit: their tops are dear, it keeps to the ground |
| CC_B08 | the pipe at the bottom to the right hand pocket, up pole x=43, off it onto the beam at y 11, from the beam's left end into the outer wall and wall jumps up the **left** gap beside the block (a player's recording: the right gap's walls start too high, and the room is open past its right edge), the block's pipe to pole x=20, then pole x=20 → the ground at (25,17) → pole x=27 → ... ; **fails** in the top left: the jump from the y 64 corridor up to the ledge at (20,67) |

It gets to CC_B08 after about 8500 ticks (≈ 3.5 minutes; vanilla players
are a lot faster) and never dies before it. A player's recording of CC_B08
(`npc_human_HI_S05_UW_S04.txt`, a Watcher) replays to PASS with
`--npc-human-complete HI_S05 UW_S04 --script "<its last line>"`.

## The whole world

`--npc-soak 3000` with the default share (Survivor's world): 103 NPC
slugcats in 13 packs. 40 of them have no shelter they can reach (their
shelter's surroundings need moves the graph doesn't have). In 3000 ticks:
6 shelters reached, 56 region crossings counted, 76 trips, 322 rises, 335
running jumps, 43 pounces, 20 flights, 7 stuck; 4 dead (a White lizard,
three long legs). The world ticks at 1.28–1.55 ms a tick on 6 threads with
them, about what it took without (the "no way" counter counts NPC ticks
without a plan, not events).

## Observations

Things learnt on the way, mostly about the port's Player.cs:

- Against a wall (`WallClimb`) a jump is a wall jump *away* from it: to
  jump up to a ledge, step off the wall first.
- A wall jump needs the slugcat pressing into the wall for a few ticks (it
  sets `canWallJump`) and then a fresh jump press; turning away and pressing
  jump on the same tick does nothing.
- On a pole a sideways jump goes off the side the slugcat *faces*; facing
  the other way it needs a few ticks of the direction first, and jumping on
  the same tick as turning doesn't leave the pole.
- At a pole's top the head stays a few pixels under the top tile, so the
  top tile is never "where it is" by its head.
- Up on a beam, pressing down drops through it; jumping from it while
  holding up catches it again.
- Stepping sideways off a pole onto a beam stands on it only if the feet are
  at the beam; with the head there it hangs.
- Hanging under a pole (`HangUnderVerticalBeam`) neither chunk is on the
  pole's tiles: it has to press up to climb on.
- Coming out of a pipe whose mouth is a corridor, pressing up pushes it back
  in; pressing down does nothing for a while.
- Falling out of a room's bottom doesn't kill (the game does).
- Holding a slugcat's input still for the 6 ticks of a turn makes most
  moves reliable; most failures were a tick or a tile off.
- From a player's recording (CC_B08): jumping off a pole's side goes
  further started as up-and-towards 2 ticks, towards 2 ticks, then jump
  held 12, reaching up only some 20 ticks in (`pole_jump_reach` is measured
  that way). Holding up while still on the pole turns the buffered jump into
  shimmying up the pole. At the pole's top, or where a horizontal beam
  crosses it, holding the direction first lets go of the pole or hangs from
  the beam: there it jumps at once.
- A jump up to a ledge from the ground: a tick towards it, then jump
  without up (up together with jump spoils it), up later to climb on; from
  the column it rises in, not one under a ceiling next to it.
- Outside a room is what its edge tile is: a room open at its side is open
  beyond it (a wall jump shaft there has no wall).
- A wall jump climb has to find its first wall level with its head; a shaft
  that starts two tiles up with no wall below sends it flying sideways.
- A jump to a pole should end on any beam caught right by the pole; a
  flight should let go of jump once it grabs or crawls onto a ledge (still
  holding it jumps off the far side).

## Not done

- **CC_B08**'s top left (above): the jump from the y 64 corridor to the
  ledge at (20,67) a player makes with `L1,LJ7`.
- 40 of 103 NPCs in Survivor's world can't reach any shelter; 423 exits
  lead nowhere. Missing: wall jumps up single walls (Rivulet), wider
  shafts, jumps from beams other than straight up, rocket jumps, belly
  slides, flips, swimming routes through deep water (water is walkable, but
  nothing about breath), slopes beyond plain steps, the Watcher's abilities,
  Saint's tongue, Spearmaster's spears as steps.
- Every slugcat moves with Survivor's reaches. Rivulet, Gourmand, Saint
  and the others jump differently: the reach tables should be per slugcat.
- No karma gate checks (karma isn't tracked), no food, no sleeping (it just
  rests in the shelter; the shelter doors aren't modelled).
- Dynamic warps are planned with where they led at world load.
- Packs: members re-plan on their own after fleeing; the leader only waits
  for members in its room.
- The trip test runs one slugcat; the rest of the world with NPCs is only
  soaked, not checked room by room.
