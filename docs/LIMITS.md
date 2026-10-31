# Limits

What rwcpp assumes about the world, so that code can rely on it.

## The scenario is fixed while the world ticks

No region, room, creature type or slugcat type is added or deleted while the
game is running. More generally, objects tagged `scenario` in
`dcon_generated.txt` can be assumed never to be added or deleted while the
world is ticking. They are what the world files describe, loaded before the
first tick:

- the regions, rooms, palettes and room cameras, and the region and camera
  memberships;
- the tiles;
- the room exits, their connections, the shortcuts and the room nodes (with
  the gates' locations);
- what is placed in the rooms: placed items, rot, warps and warp targets,
  decals, music triggers, ambient sounds and other placed things.

All of these use `storage_type{contiguous}`: their ids run from 0 to the
count, with no holes.

Creature types (`creature::type`, `creature::type_count`) and slugcat types
(`player::slugcat_name`) are compile-time enums, so their counts never change.

What this allows:

- **Buffers sized once.** A `ve::vectorizable_buffer<T, id>` over one of
  these objects is built with its constructor from the count
  (`w.room_size()`, `w.region_size()`), which is the only count needed: no
  stored size, no bounds check, no reallocation per tick. Reading an invalid
  id (no room, no region) lands in the buffer's leading pad. What is written
  before it is read needs no clearing; a tally (`++`) writes its zeros over
  every id first, a vector at a time (`execute_serial_over_room` and
  `set(ids, ve::int_vector{0})`). `int32_t` and `float` lanes store at full
  width, while ve's 8- and 16-bit stores go lane by lane and have no masked
  partial form.
- **Whole-world passes** over them (`execute_serial_over_room`,
  `execute_parallel_over_room` and so on) see the same set of ids every tick.

Not covered: the objects tagged `save`. Creatures, items, players, body
chunks, rot meals and the rest are made and destroyed while the world ticks,
through the deferred transactions after the rooms' pass. Buffers over those
must be sized when they are used.

These buffers live in the world (`game_state`, e.g. `npc_buffers`), not in
function-local statics: one process holds many worlds (the selftests, the
bench's runs). They are built in `simulation::populate`, where scenario
objects come in. The game populates once before its first tick; the
selftests add rooms between scenarios, always followed by a `populate`
(every tick starts with one), never inside a tick.
