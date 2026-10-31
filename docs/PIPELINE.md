# The tick as a parallel pipeline

rwcpp simulates the whole world every tick (every room, every creature). This
document models `simulation::tick` as a pipeline of **stages**. Within a stage,
**workloads** that touch disjoint data run at once, and the whole-world work in
them runs as dcon vector passes. It records what was measured, what was tried
(including what didn't pay), and the rules for moving more work into the
pipeline.

## The model

```
stage 0   rain::update_world ‖ npc::update_world            (concurrent workloads)
serial    populate, the vessels in pipes, cameras, rooms::build_index
stage 1   every room's update_room                           (tbb::parallel_for, auto partitioner)
stage 2   items::update_counters                             (whole-world dcon vector pass)
serial    the rooms' deferred transactions, merging their outputs, warps, particles
```

- **A stage** is a set of workloads with no data dependency between them. It
  runs as one `tbb::parallel_for` over the workload indices with a
  `simple_partitioner` (one task each), and a `switch` on the index picks the
  workload (`stage()` in `simulation.cpp`). Put the heaviest workload first so
  it starts first: the stage lasts as long as its longest workload, and the
  imbalance is tuned by an educated guess at each workload's cost.
- **A whole-world pass** handles one object kind across the whole world (every
  item, say) in dcon's vectors (`execute_serial_over_<obj>` /
  `execute_parallel_over_<obj>`) instead of per room, per object. It reads and
  writes through tag gathers and scatters (`ve::fp_vector`, `ve::int_vector`,
  `ve::tagged_vector`), and branches become `ve::mask_vector` +
  `ve::select`.
- **A carve-out**: when a pass `X Y Z` has one step `Y` that depends on the
  per-kind payload union (`creature_get_payload`), `Y` runs inside the
  vector loop through `ve::apply` (scalar per lane) with a `switch` on the
  kind, rather than splitting the loop into three. The other steps stay
  vectorized; only `Y` serializes. Use it sparingly, since every carve-out
  costs a lane-by-lane pass. Note that `ve::apply` runs the lanes in *reverse*
  order (15 down to 0), so the carved-out step must not depend on lane order.
  The stateless RNG (below) guarantees that for its rolls.

## Measurements

All numbers are from `--bench`, on 6 cores with AVX-512, built with
`-march=native` (FMA on). Baseline: commit `049b23a` built with the same
flags. Single-thread runs vary by about ±15% from run to run on this machine,
so compare several runs, and compare the world hash: while no divergence is
intended, the hash must not move.

| mode | build | 1 thread ms/tick | 6 threads ms/tick | world hash |
|---|---|---|---|---|
| each | baseline | 1608 | 325 | b69c1b2e527595ee |
| dens | baseline | 11.2 / 11.9 / 11.5 | 2.54 | 2a952ae48fa59341 |
| each | exact changes (pull loop exit, ve water) | 1651 | 329 | b69c1b2e527595ee |
| dens | exact changes | 15.0 / 13.0 / 10.8 | 2.81 | 2a952ae48fa59341 |
| each | + stateless RNG, stage 0, item counters | 1594 | 302 | 37043ff22e381815 |
| dens | + stateless RNG, stage 0, item counters | 11.1 / 10.8 / 10.7 | 2.41 / 2.39 / 2.44 | 969ebe41bebc237a |

The exact changes leave the hashes untouched under native flags (and under the
old SSE build: `812f0ad2d8667a75` before and after), and the difference in
time is within noise. With the stateless RNG, stage 0 and the item counters,
6 threads come out about 5 to 7% faster (each 325 → 302 ms, dens 2.54 →
2.41 ms) and 1 thread about the same. The world differs from then on (other
rolls, so the dens make 934 creatures instead of 922), so these are
comparable only roughly. 1 thread and 6 threads still agree.

Six selftests failed after the switch. They were scenarios tuned to
particular rolls (the blue lizard test, for one, reseeded the old generator
to 99 so that its first lash hit). Under 8 world seeds, 4 of them pass for
some seeds and fail for others, and one seed passes 113 against the
baseline's 112. The blue lizard one fails for every seed: its first lash
misses, then it bites and carries the slugcat home, a valid hunt the test
doesn't count.

Where the time goes:

- **Stage 1 (the rooms) is 97 to 99% of the tick.** The serial parts were 0.07
  to 0.25 ms before and 0.02 to 1.6 ms after it (each mode). So stages 0 and 2
  have little to gain on their own: the pipeline only pays off by moving work
  *out of* `update_room` into concurrent workloads and whole-world passes.
- **each mode (one of every kind per room): the daddy long legs.** Its
  `Tentacle.PullAtTentacle` (`daddy_tentacle.cpp`) is about two thirds of the
  game's own CPU time, all of it in `ray_trace_tiles_for_terrain`. Over 60
  ticks it made 230k calls with 142k candidate pulls, whose inner check (does
  every segment between see every tile of the new straight path) made
  **251 million** `visual()` traces, nearly all passing. It is
  O(segments² × path) per pull.
- **dens mode (the world files' population):** a flat profile. The largest are
  `water::step` 5.4%, `pathing::field_to` 4.4% (11.6 fields built a tick),
  `physics::update_chunk` 1.9%, worm grass, `update_item` 0.8%, rot.

## Findings

### dcon's parallel loops are for SIMD bodies, not scalar per-room code

`execute_parallel_over_room` replaced the room loop's `tbb::parallel_for` with
`update_room` called once per id of each tag block. 6 threads got slower:
each 279 → 364 ms, dens 2.46 → 3.33 ms; 1 thread unchanged. The reason is that
`ve::execute_parallel` / `execute_parallel_exact` hand out blocks of 16 ids
under `tbb::static_partitioner`, so a block that happens to hold a daddy long
legs can't be stolen by an idle thread. dcon's loops pay off when the body is
SIMD work of about the same cost per lane (gathers and scatters through the
tags). A heterogeneous scalar body like `update_room` stays on TBB's balancing
partitioner. Reverted.

### Exact rewrites

- `water::step`: the inner columns in `ve::` vectors, the walls as a
  `mask_vector` choosing the node's own height and zero speed through
  `ve::select`, the edge columns scalar so that no neighbour read leaves the
  row. Same operations in the same order: bit-identical (the hash didn't move).
- `PullAtTentacle`: once one inner `visual` fails, `flag2` can't turn true
  again, so leave both loops (vanilla's `break` leaves only the inner one).
  Same result, fewer traces. It made no measurable difference, because nearly
  all inner checks pass.

### Tried, and reverted

- A per-room summed-area table of solid tiles as an O(1) "is the box between
  the ray's ends clear" test in `Tentacle.Visual` (exact: this Bresenham walk
  never leaves the box of its ends). No measurable gain: tentacle segments lie
  along the terrain, so nearly every box contains a solid tile, and the clear
  cases were already caught by the terrain proximity early-out.

### Native flags

`ve` takes its width from the target (`ve.hpp`): SSE 4 lanes, AVX2 8, AVX-512
16. The build used to target the x86-64 baseline. `RWCPP_NATIVE` (on by
default) adds `-march=native`. GCC then contracts `a * b + c` into FMAs: an
accepted divergence (DIVERGENCES.md). Determinism holds per build: 1 thread
and 6 threads agree.

### Stage 0: rain ‖ NPC packs

`rain::update_world` writes the regions' cycles and, on a new cycle, the den
creatures' `remain_in_den` and their `random` (`dens::cycle_restarted`).
`npc::update_world` writes the player slugcats' packs, rides and their
creatures' `random`. Their objects are disjoint. Both only *read*
`state.random.state` and region membership, neither pushes sounds or logs
outside a room, and `crowds()` is npc's own. So they run as two workloads of
one stage.

### Stage 2: item counters as a whole-world pass

`Weapon.vibrate` and `PlayerCarryableItem.forbiddenToPlayer` tick down in
`items::update_counters` for every item at once:

- **Which items:** the ones `update_room` went through, those whose body is in
  a room. The mask is gathered as `item_body` → `object_location` → room,
  compared with the invalid room. It runs before the deferred transactions, so
  nothing has changed room, been made or been destroyed since the index was
  built.
- **Timing audit (the method for any counter moved into a pass):** list every
  setter and reader, and where each runs relative to the old decrement.
  - Readers: `forbiddenToPlayer` is read by the player's pickup, which runs
    before the items in the room pass. The value it sees is unchanged.
    `vibrate` is read only by the renderer.
  - Setters before the old decrement are still decremented the same tick:
    `grab`, `throw_object`, `creature_grab`, `creature_throw`, the garbage
    worm and `hit_wall` all run in the player and creature updates or earlier
    in the item's own update. These keep their timing.
  - Setters after it would now be decremented one tick early. The only one is
    `Spear.Update`'s 10 ticks of `vibrate` when lodged: 9 now (DIVERGENCES.md).
- `needle_fade` stays in `spear_update`: its condition (`needle == 2`) is set
  later in the same update, so a pass would decrement it a tick early.

### The stateless RNG

Vanilla rolls `UnityEngine.Random`, one serial stream, and the port used to
keep a generator per creature, item, room, region and the world. Now no
generator is stored anywhere. A roll is a pure function (`rw::random` in
`math.hpp`, `keyed_random.hpp` for `ve`):

```
key  = seed ^ tick * K_tick ^ id * K_id ^ salt * K_salt
roll = lowbias32(key ^ n * K_draw)
```

- **seed:** the world's, `game_state::seed`. It is set when the world is
  made (main mixes in the clock before loading) and never changed.
- **tick:** passed explicitly (`state.tick`). Use 0 for what must come out the
  same whenever it's rolled: a creature's looks, a shape drawn every frame,
  the layouts made at a room's load.
- **id:** what tells the roller apart: the dcon id's `.value` (a creature's
  identity is its id; nothing is stored for it), or `rw::ids(a, b)` for an
  object and the one it rolls about.
- **salt:** the call site (`rw::site()`: file and line, through
  `std::source_location`), automatic. Two rolls of a tick at two places are
  unrelated.
- **n:** which draw of that call. `rw::rng` is a value made where the rolls
  happen, holding the key and the draws taken, and it is dropped after.
  Helpers take it by reference.
- Each input is multiplied by its own odd constant before the XOR. A bare
  `tick ^ id` collides: tick 1 with id 2 is tick 2 with id 1.
- **The hazard:** the same site, tick and id give the same numbers. A helper
  called several times per tick for one object takes its caller's `rw::rng`,
  or puts the loop's index or the other object's id into the key.
- `keyed_random::key(seed, tick, ids, salt)` and `draw(keys, n)` compute in
  `ve::int_vector` the same bits as the scalar (a selftest checks every lane),
  so a whole-world pass rolls for 16 objects at once. `ve::int_vector` has no
  shifts, so `shift_right` uses the target's intrinsic on `.value`.
- What it buys: no roll waits on another object's, so a whole-world pass, a
  `ve::apply` carve-out (lanes in any order) or a stage's workloads can roll
  freely. The rooms' shared streams (`room_random`, the rooms' `cosmetic`)
  are gone too: a roll in a room keys by the object it is about, so the
  objects of a room no longer serialize through one generator.

## Rules for adding a workload

1. **Data:** list what it writes and what it reads. Two workloads share a
   stage only if neither writes what the other touches. Different elements of
   the same SoA array are fine; the same element, or a container's size, is
   not.
2. **Side channels:** with `state.parallel == false`, `play_sound`, the
   particles and `reports::add_log` push to shared queues. A workload in a
   concurrent stage must not use them, or must use per-workload buffers merged
   after the stage.
3. **Timing:** moving work out of `update_room` into a pass before or after it
   shifts it relative to everything else in the room's tick. Audit the
   readers and setters as for the item counters, and note any shift in
   DIVERGENCES.md.
4. **Randomness:** roll `rw::random(state.seed, state.tick, <the object's
   id>)` (or the vector `keyed_random`), never a stream kept between calls.
   The order inside the pass then doesn't matter.
5. **Measure:** the stage lasts as long as its longest workload. Compare the
   hash and several runs of `--bench` before and after.

## Next candidates

- **The daddy long legs' pull check:** it's the cost of each mode. Ideas that
  keep it exact: memoize `visual(a, b)` per tile pair within one
  `PullAtTentacle` (terrain is fixed within the tick; the while loop restarts
  from `start` after every shorten and re-traces the same pairs); or trace the
  path once per segment `k` as a fan, stopping at the first failure.
- **Path fields** (`field_to`, 11.6 a tick in dens): building them is pure
  (room geometry + goal → distances). Fields requested this tick could be
  built in a stage running alongside the rooms and used from the next tick,
  with one tick of latency as the divergence.
- **Creature counters** (stun and the payloads' cooldowns): a whole-world pass
  over creatures, with the payload counters in a carve-out (`ve::apply` +
  `switch` on the kind). Each kind's counters need the timing audit first.
- **Body chunks:** `physics::update_chunk` integrates (vectorizable) and
  collides with the terrain (scalar per chunk). Split into a vector
  integration pass and the per-room collision, after an audit of who moves
  chunks in between.
- **Water:** each room's `water::step` could run as one whole-world workload
  over every surface, concurrently with something that doesn't touch water.
