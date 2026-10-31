# NPC slugcats (SlugAI)

Slugcats without a controller, driven by a brain of our own. Vanilla's
`SlugNPCAI` is not ported and not used. This document covers what they do,
how the code is laid out, the movement model and the measurements behind it,
the tools for tracing them, and what is known not to work yet.

## What they do

- **Spawning.** At world load a share of the shelters (`--npcs <share>`,
  default 0.25; `--no-npcs` for none) each get a group of 1 to 8 slugcats,
  rolled with the shelter's own generator (`npc::spawn_world`), around
  the shelter's sleeping spot (below). Each one is
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
  [Routes](#routes)). In the target it walks to the sleeping spot (gives
  up after 600 ticks: a crowd, a spot it can't reach) and rests until its
  region's rain cycle is over (`rain::cycle::cycles` changes; the shelter's
  *region*, a shelter has no room rain of its own), or for 4800 ticks in a
  region where it never rains, then picks another one. When the rain is
  less than 1600 ticks away it makes for the nearest shelter once a cycle
  ("hurried").
- **No walkable way to any shelter** (the path graph lacks a move its
  surroundings need): it doesn't stay. It picks a shelter over the rooms'
  connections alone, as if every pipe of a room could be walked to from
  every other (`explore(..., any)`), and tries (`nav_state::guessed`).
  Where the walkable way does go on from how it got into a room, the guess
  keeps to it: only rooms the walkable way leads nowhere from are taken as
  open. WRFA_E04's gap (no jump closes it) isn't guessed across from the
  WRFA_A12 side, where the walkable way leads back to A12's warp. Into a
  new room on a guessed trip (through a warp, say) it looks again: a
  shelter it knows the walkable way to now (not the one it set off from)
  becomes its target ("retargeted").
  - **Learning where a guess fails.** Stuck 300 ticks in a room on a
    guessed trip, it remembers the room (the last four) and guesses
    through it no more: only the walkable way there. If the careful guess
    then finds nothing, a wide one takes every other room as open (from
    WRFA_S08 the walkable way out of WRFA_C11 led only to WRFA_E04's gap).
  - **Diverting to a warp.** Picking a new target, not the shelter it set
    off from: with no other shelter it knows the walkable way to, it makes
    for the closest warp it can walk to and picks again on the other side
    ("diverted"); then a guessed shelter; home last.
  - The push towards a pipe it has no walkable way to never steps towards
    a column without something to stand on within 6 tiles down, and jumps
    only at a wall in front (it walked off WRFA_E04's slope into the pit).
  Where the path field doesn't reach it, it makes for the next pipe
  straight (up to it, jumping now and then for ledges, walls and poles,
  never towards a column with nothing under it to the room's bottom). No
  progress: the usual replanning, and after three failed trips another
  target; the one it gave up on last only when there is no other.
- **Chased back.** Fleeing back through a pipe it hides where it came out
  before trying again: 400 ticks more each time in a row it is chased back
  from the same room (up to 8), plus up to 200. A creature camping by the
  only way out (GW_S06 into GW_A04) moves on in the end; going out and
  straight back every 150 ticks it looked like it never left.
- **Falling out of a room** kills it (Creature.Update: the head under
  −500 px, −250 in a wall climb, in a room without water). The port doesn't
  kill a slugcat for it otherwise; an NPC down there waited under the room
  for good.
- **Warps** go as for the player (warps.cpp): it walks under the tear and
  stands still in its grab (any input holds a warp back) until the warp
  sets off, is pulled into it, comes out of the exit tear hovering. Any
  NPC slugcat may set a warp off (the player's slugcat only as the
  Watcher). The warp carries the one that set it off and the slugcats in
  its grab (`pull_radius`), no other slugcat in the room. The screen wide
  transition (and the camera's move to the other room) plays only when the
  player's slugcat is among them; without it the hover lasts the
  sequence's second half. Each warp keeps its own pull
  (`game_state::warp_pulls`), so warps in different rooms go at once. A
  tear out of reach from the ground: after 400 ticks of waiting it asks
  the warp to take it (`warps::carry`, every tick from then on).
- **Warps for every creature** (not vanilla): `warps::carry` is a
  request. A ready warp sets off (the whole sequence, the suck in and its
  timings) and carries the creature and what it holds, wherever it is in
  the room; a warp opening, cooling down after a use or closed carries
  nothing, the creature waits. Something waiting 3 s by a warp that cools
  down opens it again (`warp_point::wait_ticks`; only what asks to go
  through counts, so a slugcat that just came out of it standing about
  doesn't send it back). An activated warp takes everything in its grab,
  wanted or not.
- **Strays.** An NPC carried by a warp it didn't mean to take (not its
  own dynamic warp, not the next hop of its trip) without its leader
  leaves its pack (no pack: alone, a new target from where it came out;
  `npc::warped`, "strays"). A leader carried off that way leaves its
  members to the first of them, who leads them on to the old target.
- **Crowded shelters.** A shelter with more than 10 slugcats in it
  (`npc::crowd_limit`, counted serially each tick) is crowded: a pack (or
  one alone) on its way there and not in it yet gives it up and picks
  another; picking skips crowded shelters while there is another
  ("crowded").
- **The Watcher's dynamic warps.** A Watcher leading (or alone) that
  knows no walkable way to any shelter, before guessing, makes a warp of
  its own: it stands while its pack in the room gathers round (up to 400
  ticks), holds the ability crouched until the tear opens, then stands in
  it until it sets off. The warp (`warps::spawn_dynamic`) leads to a
  DynamicWarpTarget of another region picked when it opens, stays 30 s and
  is set off by anything that comes close, so members that didn't make it
  (or were elsewhere in the room) follow through it while it lasts
  (`follow_warp`); they stay in the pack. From the other side the leader
  picks a shelter as usual. One warp at a time (`dynamic_warp_cooldown`,
  30 s; 30 s more after a failed try, e.g. on a pole). Not under
  `--npc-guess`. Tests: "NPC Watcher takes its pack through its own warp"
  (with a stray from another pack pulled along).
- **Falling to a certain death** (pushed off, a jump misjudged):
  `predict_fall` follows its fall tick by tick as the physics moves it
  (gravity, air friction). Out of the room's bottom with no water there,
  or onto the ground faster than 60 px a tick, is certain; water on the
  way or a ledge beside the fall it might catch, and it isn't. Only when
  it is certain (`catch_fall`, before anything else it thinks) it drops
  its move and holds up (the player code's beam grab) steering for the
  beam its head can get to first (within four tiles, a few pixels of air
  control a tick). Hanging from a beam with no step on, it pulls itself up
  onto it. Test: "NPC catches a beam falling to its death" (a pole by the
  pit: lives; none: dies; over the ground with a pole by it: no grab).
  Stats: "catches".
- **Crowded rooms.** A room with more than 5 slugcats: every 400 ticks
  (each NPC on its own tick of them) each pack member there (not resting)
  may leave its pack, 5% with 6 slugcats and 2.5% more for each one more
  (`room_crowd_limit`), and make for another shelter alone (the target it
  had given up; off a back it rides on; a leader's pack goes to the first
  of its members). Stats: "split up". Test: "a crowded room splits packs".
- **The Artificer's pyro jumps.** Its path graph has one more move, up
  its column in a jump and a pyro jump at the top, onto a ledge beside the
  column (`step_kind::pyro`, `pathing::pyro_rise`: 7 tiles up one over, 6
  two over; the feet go 8.5 up straight, measured). Only the Artificer's
  fields have them (`pathing::artificer`, keyed apart in the cache), and
  the route table keeps its walkable masks apart (`pyro_exit`,
  `pyro_arrival`, built only for rooms with pyro jumps, the others
  copied); `walking()` picks them for the NPC thinking (thread_local, set
  by `think`, `look_ahead`, `reachable_shelters`, `planned_exit`), only
  for an Artificer alone or in a pack of Artificers (`nav.pyro_pack`, set
  in `update_world`: the others couldn't follow it up). The
  move (`move::pyro`): lined up in the middle of its column standing, a
  jump straight up, the ability key pressed fresh once the feet hardly
  rise, straight up again, across onto the ledge once over it. Hot (4 or
  more pyro jumps it hasn't cooled from) it stands and cools down first: it
  never gets to the 7th (stunned) or the 10th (blows up). Test: "NPC
  Artificer pyro jumps up a wall" (a Survivor's routes don't see the shelter
  past it).
- **A Watcher in another's pack** in a region with no warps (none placed,
  no dynamic one open) leaves the pack, to make its own way. A Watcher
  leading keeps its pack.
- **Into openings.** A jump may end in a pipe's mouth (in the air in
  front of its entrance: pushing on, it goes in) or at a passage's end (a
  crawlway one tile high it goes into sideways, a corridor it goes up into
  from below): the arc only has to reach the tile before it (UW_D02's exit
  high in its wall, from pole x=9's tip). A tile with rock on all four
  diagonals is walkable (corridors meeting).
- **Beam ladders, pole hops.** Straight up from a beam's top onto the beam
  over it, unless walls close on both sides make it a narrow shaft (HI_C11
  is wall jumped). From a beam's top a hop onto a pole up to 4 over and 1
  up; from the ground up to 5 up when the pole is 1 or 2 over (UW_E03's
  pole x=9). Rising to a pole it reaches up as soon as its hands are by it.
  The tip move only off a vertical pole it holds.
- **Out of a crawlway** (a tile one high, its open end over air): straight
  across onto a pole 2 to 4 tiles out, level or a little lower (UW_E03's
  tunnels up its left wall onto pole x=9, twice).
- **Bonking.** A jump off a pole rises first, however far down it lands:
  it needs two tiles of room over its head beside the pole where it leaves
  (the planner's arc for a long falling jump started downwards, under a
  ceiling the jump hit and dropped short: UW_E02's pole x=39 under its
  block). And under a ceiling within three tiles it doesn't start with the
  player's up-and-towards ticks (holding up climbs the pole, the jump leaves
  higher and bonks).
- **Pole to pole** up to nine across (falling eight or more); coming down
  onto a pole it aims where there is pole under it too, or at the pole's
  very foot, caught hanging under it (a player's UW_E02: pole x=39 to
  x=31's foot, twice). A sideways jump from a pole's tip onto another pole
  (the player did that too) was tried and dropped: CC_B06 then took it
  over its working jump-and-bounce and got stuck.
- **Crawl-turn vault** (`vault`; the community's "perfect CTV", see the
  Rain World wiki's Crawl-turn vault page): on flat ground with open air
  over it, ahead and behind, onto a pole 1 to 3 ahead and 3 to 6 up. Face
  the way, down on fours, then: back 5 ticks with up at the second (the
  crawl-turn, cancelled), down + the way + jump at once, the way + jump 6,
  up + the way + jump 10, up till it holds. Measured in UW_E03 with
  `--npc-exp uw_e03,7,8,S,R2,N4,D12,L1,UL1,L3,DRJ1,RJ6,URJ10,U30`: the feet
  rise 6.4 tiles, the head passes row 15 at x≈9.8 and catches pole x=9 at
  its foot (UW_E03's way in; a straight jump, a single wall jump: row 12).
  It lines up 7 px back of the tile's middle first (where it was measured);
  failed twice from a tile it isn't tried again in that room (the usual
  blocks are let go every 200 ticks: back and forth).
- **Spear steps** (`npc.cpp`'s `find_spear_step`, `spear_way`), only where
  no other way on is known: in a room whose path field doesn't reach it
  from where it is (looked for once a visit), a throw straight at a wall
  within 8 tiles (from that close a spear always sticks: within 140 px of
  where it leaves the hand) from a tile it can get to, standing on the
  ground or holding a pole (the head's height), whose stuck spear's beam
  (the tile by the wall and the one behind it) ends near what reaches the
  goal; each tried for real (the beam set, the way looked for, the beam
  taken away), the closest throws first. Only with a spear to be had: in
  its hand, or lying where it can get to. Then: the spear first (fetched,
  picked up walking over it), to the spot, the throw (on a pole one tick
  of the way with the throw: it holds on), the beam waited for, on its way
  (a miss: fetched again, three throws). Throws at threats wait meanwhile.
  A guessed trip takes a room with a spear placed in it (ReliableSpear) as
  maybe open between all its exits. UW_D03: from the top of pole x=25's
  lower part into the wall at x=33, the spear sticks at (32,145); from pole
  x=30's tip onto it, up the wall beside it into a ledge grab onto (33,148).
  Tests: up a wall with a spear lying on the way; no spear, no step; on the
  pounce course a spear in its way stays where it is.
  - **Looking ahead** (`look_ahead`, serial in `update_world`, again when
    its trip changes): a room on the trip with no walkable way between where
    it comes in and where it leaves but a spear step (as if it held a spear)
    needs one. A spear to be had on the way there (in its hand, lying in a
    room before or in that one, or a ReliableSpear placed there): fetched
    first, in whatever room it is in (`spear_needed`), and kept (not thrown
    at threats). None: it can't make it that way, the target is given up
    and another picked ("gave up"); the same in the room itself when a
    spear would do and none is to be had. Tests: carried from the shelter
    it starts in to the room that needs it; no spear anywhere, another
    target.
- **Grapple worms** (tube_worm.cpp): a held worm's tongue, shot with a
  jump in the air, sticks within 230 px and the slugcat swings 140 px under
  it. In UW_B01 from the floor at (92,18) it sticks under the block pole
  x=93 stands on and hangs at row 22; pumping the swing doesn't lift it
  past the block. Not used by the NPC yet: B01's way with it is still to
  be found.
- **Platforms over pipes.** A jump never lands in a platform or a pipe's
  entrance tile; a jump into a pipe's mouth only into one facing it from the
  side (UW_E03's pipe at (29,89) opens up under a platform: drop through
  it). Coming down into a pit, a running jump or a pounce counts a wall as
  stopping it only if it is the body's height (the pit's one-tile edge with
  slopes over it: it flew over).
- **Back and forth.** Going back into the room it just came from three
  times in a row (each room's way on leading through the other): on a
  guessed trip neither room is guessed through again; on a walkable one the
  target is given up and another picked.
- **No wall jumps on a pole**: holding a pole that goes on up, or with a
  pole up the shaft, it climbs (WRFA: up the pole into a pipe).
- **Jumps that carry too far.** A charge pounce or a running jump goes its
  full length: one planned onto a ledge short of that needs a wall to stop
  against or ground all the way past it (WARG_B35's open side sent slugcats
  out of the room).
- **Shafts three wide** (wall jumps between their walls): each climb is
  found from the start tile under any of the shaft's columns (under the
  right side of WRFA_C11's upper shaft its left column is two over, and the
  shaft was missed). With it WRFA_S08's slugcats go up C11's two shafts to
  WRFA_A21 and its warp instead of to E04's gap.
- **Beams.** Hanging from a horizontal beam it gets up on it with a fresh
  press of up (Player.cs wants `input[1].y == 0`; held from the jump that
  caught the beam it hangs there for good). Up on a beam it jumps straight
  up into a corridor or onto a pole over it (lined up under it in nudges, a
  one tile shaft takes no less; not holding up), never onto another beam
  that way. WRFA_S01's way out is that: up to the beam, onto it, up the
  shaft.
- **Sleeping spot** (`npc::shelter_spot`): ShelterDoor's `playerSpawnPos`.
  From the shelter's exit pipe entrance out (the first of left, up, right,
  down that isn't solid) until the next tile is solid, going up also left to
  the wall, then down to the floor. A `ShelterSpawnPoint` placed object
  would override it; the port doesn't parse that object yet.
- **Turning back** (a failsafe, not vanilla). Each pack rolls a share of
  the cycle, 50 to 75% (`nav_state::turn_back`, the leader's counts). Once a
  cycle, when the cycle passes it, the leader counts how many rooms are
  left to the target against how many there were when it picked it. Less
  than half way there (or no way on from here), it goes back to the shelter
  it started in (`slugcat_npc::home`); if that can't be reached, to the
  nearest one. Already in the home shelter, it rests there at once.
- **Spearmaster** starts each cycle in a shelter (waking up there, or still
  resting when a new cycle starts) by holding pick up with a free hand until
  its tail gives it a needle for each hand: the ability itself
  (`items.cpp`'s `spearmaster_update`), no item made by the brain. Once a
  cycle, giving up after 400 ticks.
- **Fleeing.** A creature that would kill or eat a slugcat (that
  creature's own dynamic relationship to it: `go_for_kill`, intensity at
  least 0.05), in sight and within 360 px, sends it running. Not a
  squidcada or a leech (it ignores those), not one that is stunned (it runs
  past). **Rivulet** runs from nothing. The others have had enough after
  400 ticks of running (lately: it counts back down while it isn't), or
  when chased back into the same pipe a second time (a short wait first):
  for 600 ticks they ignore every threat and go on their way ("braved").
- **Weapons.** Walking over a weapon (rock, spear, bomb, lilly puck: lying
  free within 30 px of its head or feet) with a hand free it taps pick up;
  it never goes out of its way for one. With a weapon in hand and a threat
  in sight, within 400 px and level with it (60 px), it turns to it and
  throws (every 20 ticks at most). Thrown items hit creatures as the
  player's do; a stunned or dead threat is no threat, so it runs on.
  Spearmaster throws its needles the same way.
- **Spearmaster fights.** A threat doesn't send it running: it goes up to
  where the threat is level with it and in range, throws what it holds,
  and with its hands empty holds pick up for a new needle out of its tail,
  backing off while it comes out when the threat is close (the pull works
  on the run, up and down let go). A stunned threat stays its target; it
  stops when the thing is dead, or after 1200 ticks of fighting one it
  can't get at (then on its way, deaf to threats a while).
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
start their screen-wide sequence for an NPC (only when the player's
slugcat is in the pull); an NPC goes through a warp's sequence as the
Watcher does, or asks to be carried (`warps::carry`) like a creature. The HUD's meters and the ripple layer look at the first
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
  away), stand up (up only when not standing yet: by a pole it grabs it),
  facing the ledge (a jump mid-turn hardly leaves the ground), then jump
  towards the ledge holding it, and hold up. Two or more up beside the
  ledge it takes off from the far side of its column (4 px off the middle),
  clear of the ledge's corner. Back on the ground 3 or more tiles off where
  it started, the jump fell short: blocked, not walked on under a ceiling.
  From a beam's top it doesn't hold up for the first 22 ticks (it would
  catch the beam again). Holding a pole and rising straight up, it does a
  tip jump instead.
- `back_up`, `run`: two tiles back for a run-up (unless it is already
  running that way), then jump at the edge and hold it.
- `crouch`, `charge`: down on fours facing the gap with both ends on the
  ground, hold jump until `superLaunchJump` is 20, let go. Still coming up
  out of a pipe or a corridor below the start, it climbs up first (down
  there it would go back in); where a pole goes through the start tile it
  steps off towards the gap and crouches beside it (down on a pole climbs
  it).
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
| `--npc-trace <room\|*>[,every]` | trip, human-complete, selftest, soak (there: every NPC in the room, one line each) | input, `can_jump`/`want_to_jump`, facing, move timer, describe, the step taken, aim, goal, best distance, stuck, body mode, animation, head and feet every few ticks while in that room |
| `--npc-route <r1,r2,...>` | trip | `ok`/`NO` for each room of a route: can the way out be walked from the way in |
| `--npc-maps` | route | for each `NO` room: its poles and a map, `f` reachable from the way in, `o` can reach the way out, `B` both. The gap between `f` and `o` is the missing move |
| `--npc-room <room>` | soak | the room's poles, pipes (type, ends), map (`.` walkable) |
| `--npc-goal <bit>` | room | mark (`o`) what reaches that exit/warp |
| `--npc-box <x0,y0,x1,y1>` | room, goal | the walkable tiles there: stand/walk, pole/hbeam/pipe, distance |
| `--npc-edges <room,x0,y0,x1,y1>` | soak | the jumps leaving those tiles: kind, landing, cost, side |
| `--npc-dead-ends` | soak | the exits from which no other exit can be walked to |
| `--npc-unreachable` | soak | for every NPC with no shelter in reach: its room and the rooms its search reaches (the first three: their route data) |
| `--npc-list` | soak | every NPC at the end |
| `--npc-guess` | trip, human-complete | set off even with no walkable way known (a guessed trip, retargeting on the way; getting stuck or finding no way doesn't fail it, it learns from it) |
| `--npc-measure` | `--selftest` | the movement measurement harness instead of the tests |
| `--npc-exp <room,x,y,S,script>` | trip | standing on tile x,y, the script (`--script`'s letters, `X` throw and `P` pick up included) played, every tick printed, the highest the feet got, where spears stuck; `S+` puts a spear at its feet first |
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
(`--npc-route`), and the NPC walks all of it on its own: **PASS** in UW_S04
after 16052 ticks (≈ 6.7 minutes), 33 rises, 15 running jumps, 4 pounces, 0
stuck, never dying:

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
| CC_B08 | the pipe at the bottom to the right hand pocket, up pole x=43, off it onto the beam at y 11, from the beam's left end into the outer wall and wall jumps up the **left** gap beside the block (a player's recording: the right gap's walls start too high, and the room is open past its right edge), the block's pipe to pole x=20, then pole x=20 → the ground at (25,17) → pole x=27 → ...; in the top left the jump from the y 64 corridor up to the ledge at (20,67) (facing it, from the column's far side) |
| CC_A15 | along to the pipe |
| CC_C07 | pipes and poles up the tall room: no stepping sideways off pole x=22 through the platform; the ledge at (20,61) from (21,58) (from the column's far side, without grabbing the pole beside it); up pole x=39 and from (40,105) up the gap beside the slab onto (41,108) |
| CC_A06 | out of the pipe up through the platform at (27,6), off the pole and a pounce over the gap to (36,7) |
| CC_H01, GATE_CC_UW, UW_D07 | on its own |

A player's recording of CC_B08
(`npc_human_HI_S05_UW_S04.txt`, a Watcher) replays to PASS with
`--npc-human-complete HI_S05 UW_S04 --script "<its last line>"`.

## UW_S05 → UW_S07 (The Wall)

UW_S05 → D02 → E02 → E03 → D03 → D04 → B01 → UW_PREGATE → UW_S07, Survivor,
`--npc-guess` (no walkable way known end to end yet). UW_PREGATE is a
Downpour room: load `--root game/mods/moreslugcats` too, without it UW_S07
can't be reached at all (the merged world file links it only through
there). The NPC climbs D02 (≈2800 ticks), E02 (≈3100, a player's
recording taught it the foot catches), E03 (≈3400: the crawl-turn vault in,
the tunnel hops, the drop through the platform into the pipe at the top
part) and gets to D03, which is not planned: from pole x=25's lower part
(under a block at row 146) to its upper part and the ledge at (33,148) no
jump measured works (the tip of x=25, the tip and the side of x=30: the head
gets to row 145): it needs a spear step (above), and with it gets through
(≈2300 ticks), then D04 (≈1400). B01 isn't planned: its exit (93,39) is up
pole x=93, whose foot is ten tiles over the floor.

## The whole world

`--npc-soak 6000` with the default share (Survivor's world): 103 NPC
slugcats in 15 packs. Some of them have no shelter they can reach (their
shelter's surroundings need moves the graph doesn't have). In 6000 ticks:
19 shelters reached, 73 region crossings counted, 126 trips, 659 rises, 435
running jumps, 102 pounces, 23 flights, 52 stuck; 5 dead (four long legs,
one fall). The world ticks at about 1.7 ms a tick on 6 threads with them
(the "no way" counter counts NPC ticks without a plan, not events). Before
the guessed trips 40 of them stood in their shelters for good; now every
one sets off (no way: 0), 34 shelters are reached in 6000 ticks instead of
19, and more of them die out there (23: falls, long legs, lizards; the
falls were waiting under the rooms uncounted before).

## Slugcats with slugcats

They bump into each other (creature.cpp's `collide_slugcats`: each pair
once, their two chunks pushed apart; not in a pipe, being eaten or held),
and a slugcat's thrown weapons hit slugcats (`game_state::friendly_fire`,
on; `--no-friendly-fire` for story mode's way, through them). NPCs never
take a slugcat for a threat, so they don't throw at each other. Test:
"slugcats collide, friendly fire".

## The Watcher's ability, dynamic warps

Not vanilla's way of making them: the Watcher (player or NPC) holding the
ability (`spec`) crouched on the ground, down held, for 80 ticks opens a
dynamic warp where it is (`player.cpp`'s `dynamic_warp`; ripple rings while
it is held). With down held the ability doesn't toggle the camouflage. A
dynamic warp is a `warp_point` with `dynamic` set and its own
`warp_placed`; its destination (a DynamicWarpTarget of another region) is
fixed when it opens, so whatever follows comes out in the same place. It
fades in, any slugcat or creature close by sets it off (as the Watcher
does a placed warp), and after 30 s (`life`) it closes, fades out and is
unlinked from the room; `game_state::free_warps` keeps it for the next
one. Tests: "dynamic warp: anything close, closes after 30 s", "Watcher
makes a dynamic warp", "warp cooling down: a creature waits, forced open".

## Corpses in pipes

Not vanilla: a dead creature or slugcat in a pipe (not carried) comes back
out where it went in at once (`dens.cpp`'s vessels, `shortcuts.cpp`'s
`update_vessel`), and a corpse lying in a pipe's entrance is pushed out of
the hole. Test: "pipes spit out corpses".

## Slugpups and piggyback

**Slugpups** (`slugcat_name::slugpup`, Downpour's SlugcatStats: body
weight 0.65, run, pole and corridor speed 0.8, lungs 0.8, no throwing
skill; drawn as PlayerGraphics.RenderAsPup: the upper body closer to the
hips, the tail half as long) are a fifth of each group spawned in the
shelters (`npc::pup_share`; never rolled otherwise). `--slugcat slugpup`
plays one.

**Piggyback** (`piggyback.cpp`, Player.SlugOnBack): any slugcat on any
other's back, in stacks of any height (`on_back_of`, `back_slug`). After
the room's players move, each rider's hips go to its carrier's head
(bottom up). A rider doesn't move on its own, doesn't bump into slugcats
and isn't hit by weapons; a weapon thrown from a stack passes through
everyone in it. It goes where the bottom goes, into pipes and out of them
(`shortcuts.cpp` moves the stack with the vessel). A link comes apart when
the rider dies or is grabbed, the carrier dies, or they end up apart.
The player's slugcat takes a slugpup close by on its back with pick up
(a free hand needed), lets it off with down and pick up, and gets off with
jump; a slugpup played climbs on whoever it is by. Vanilla carries the pup
in a hand first: here it goes straight onto the back.

The NPCs (`piggybacks` in `update_world`, rolled from each one's own
generator, only while travelling): a pup climbs on a slugcat within 60 px
(its pack's first, any pack's) about every 15 s; a grown one asks a mate
of its pack close by about every 150 s; a grown one takes a pup close by
up about every 40 s. Onto the top of the stack. It gets off again after a
while (about 22 s, a pup 75 s; not in a pipe, not with a threat about). A
rider is in its carrier's pack: one of another pack joins it (the pack it
led goes to the first of its members), a carrier alone becomes a pack's
leader, a leader carried by one of its own hands the pack to that one;
carried by the player's slugcat, no pack. A rider's brain only keeps an
eye out: Spearmaster pulls needles out of its tail and throws them from up
there (`arm()`). The Artificer's pyro jump blows apart every stack with a
slugcat within 120 px, its own included (not vanilla). Stats:
"piggybacks", "off", "joined". Tests: "piggyback: the player takes a pup,
stacks", "a pyro jump scatters the stacks", "Spearmaster throws from a
back", "joins the pack, rides through pipes". Not ported: a pup held in a
hand, the throw of one.

## The Artificer

`player.cpp`'s `artificer_update` ports Player.ClassMechanicsArtificer
with Downpour's default explosion capacity (10): jump and grab (or a fresh
press of the ability key) in the air, once until it lands or holds on, is
a pyro jump (straight up 11 then a jump boost of 8, or along 15 with x
held; the burst, sparks and sound). Each one heats it a step (cooling a
step every 60 ticks, 40 when hot, once 150 passed): from 5 it sparks, from
7 each one goes higher (16) but stuns it 60 ticks more each time, the 10th
blows it up (PyroDeath: an explosion of radius 350 that kills slugcats
within 100). Not ported: the parry (down with jump and grab), zero gravity.
Test: "Artificer pyro jump, overused it blows up" (a held jump lifts its
feet 1.9 tiles, with the pyro jump at the top 8.5).

## Gates

Another failsafe, not vanilla: something over the gate's middle most of 5 s
(`held_counter` going up while anything is in the middle zones, down while
none is) starts the cycle even when vanilla wouldn't (creatures on both
sides, coming and going, slugcats throwing each other off it), for the side
more of them are on. A lone creature staying after a cycle still doesn't
set it off again (`dont_open`). Test: "region gate cycles when contested".

A failsafe, not vanilla: an airlock that stands open in the middle
(`middle_open`, both sides shut) for 5 s without everything getting through
(a creature staying in it) closes the middle door and opens the sides
again through the doors' animation, the middle shutting on whatever is
still in the way (`gate::held_counter`). The other airlock states are
already timed (the doors' speeds, `waiting`'s 400 ticks). `dont_open` then
keeps it from cycling again until the middle zones are empty.

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
- Pressing up anywhere near a pole grabs it, momentum included: a slugcat
  walking into position beside a pole and pressing up ends up on it.
- A jump turned in the same tick as it starts hardly leaves the ground;
  face the way a tick or more before.
- Rising beside a ledge, the head catches the ledge's corner unless it
  takes off from the far side of its column.
- Crouching on a pole's tile climbs down the pole; a pounce has to start
  beside it.
- A jump to a pole should end on any beam caught right by the pole; a
  flight should let go of jump once it grabs or crawls onto a ledge (still
  holding it jumps off the far side).

## Not done

- 40 of 103 NPCs in Survivor's world can't reach any shelter; 423 exits
  lead nowhere. Missing: wall jumps up single walls (Rivulet), wider
  shafts, jumps from beams other than straight up, rocket jumps, belly
  slides, flips, swimming routes through deep water (water is walkable, but
  nothing about breath), slopes beyond plain steps, the Watcher's abilities,
  Saint's tongue, Spearmaster's spears as steps (its needles are only held).
- Every slugcat moves with Survivor's reaches. Rivulet, Gourmand, Saint
  and the others jump differently: the reach tables should be per slugcat.
- No karma gate checks (karma isn't tracked), no food, no sleeping (it
  rests on the sleeping spot; the shelter doors aren't modelled), no
  `ShelterSpawnPoint` objects.
- The turning back counts rooms, not distance: a long room counts as much
  as a short one.
- Dynamic warps are planned with where they led at world load. The
  Watcher's own dynamic warps aren't in the route tables (`warps::point`
  and the tables skip them); the NPCs only use them to follow their
  leader.
- The dynamic warp has no exit tear on the other side (the slugcats come
  out at the DynamicWarpTarget without the hover).
- Packs: members re-plan on their own after fleeing; the leader only waits
  for members in its room.
- The trip test runs one slugcat; the rest of the world with NPCs is only
  soaked, not checked room by room.
