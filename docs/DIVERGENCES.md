# Divergences from vanilla

Where rwcpp's creature ports differ from Rain World (Downpour and the
Watcher on, no Remix unless a section says otherwise). The AIs of the
kinds below are simplified on purpose: they keep the behaviour a player
notices (what it hunts, flees, how it moves, how it reacts to the slugcat
and its weapons) and leave out the rest.

Shared by every kind ported here:
- Each kind's world file name is read by spawns.cpp and it spawns from its
  den like the rest; `--creature <WorldFileName>,x,y` spawns one by hand.
- The relationships (relationships.cpp) are replayed from StaticWorld.cs,
  including the rows and columns of these kinds.
- Sounds that have no id in sound_types.hpp yet are silent (noted per kind).
- Worm grass isn't a creature here either: like vanilla it's a system of its
  room (patches and worms), with no creature payload.


## Noodlefly (BigNeedleWorm, SmallNeedleWorm)

- No sounds: none of the noodlefly sound ids (trumpets, wing loops, attack, impale, bounce) are in sound_types.hpp. The cries (SmallCry, BigCry, the young's screams) only show as the snout's quiver (`screaming`).
- The AI is simplified to its core: utility comparison of threat, prey (the adult) and rain; the StuckTracker module (GetUnstuck behaviour) isn't ported, the body's own stuckAtSamePos / reallyStuckAtSamePos unsticking is. No safari control, no MSC licking, no arena "evil AI".
- Migration: no followCreature / quickMoveToExit / OffscreenSuperSpeed (no abstract layer); idle adults roll MigrationBehaviorRoll's InverseLerp(1900, 3200, inRoomCounter) / 6 into migrate::abstract_behavior; confirmedNarrowRooms (rooms it learns to dislike) isn't kept.
- The young's mother is the closest live adult of its own room (re-picked when she dies or leaves), not NeedleWormAbstractAI.FindMotherInRoom's den-based pick that persists across rooms; the young doesn't follow her to other rooms. No shelter egg imprinting (NeedleEgg), no imprinting on the slugcat.
- SocialEventRecognizer: the attacker of a noodlefly (or of a young, for its mother) is a noodlefly whose stab hit it, else the closest slugcat in the room (weapons don't carry their thrower here). The event is "lethal" for damage >= 0.5, "killing" when it died of it; recipients don't need to have seen the attacker. The adult's SocialMemory keeps 4 creatures.
- CreatureLooker simplified: it looks at keepCloseToCreature, else its focus creature (no interest scoring over every tracked creature).
- Pathing: one distance field per destination (pathing::field_to with `needle_worm::fly`: Air at 1 and CicadaPather's open air cost standing in for TravelPreference's wall cost; the +300 for stepping into a narrow space isn't there). FollowPath's look-ahead walks the field. Shortcut connections aren't followed by the body (dens.hpp pulls it into pipes); reachability (CoordinateReachableAndGetbackable) is judged by the field to its current destination. smoothedFloorAltitude is pathing::floor_altitude (not smoothed); MinFlyHeight's worm grass rule (16 over worm grass) isn't there.
- The tail segments' terrain collision is a simplified per axis point collision (SharedPhysics.VerticalCollision/HorizontalCollision, no slopes). Wind isn't applied.
- The fang's trace (TraceProjectileAgainstBodyChunks) is a line-circle test over the room's creatures (not leeches, not the young); daddy tentacles can't be impaled. Impaled chunks have no rotationChunk term. No WaterDrip sparks or screen shake on hitting terrain.
- UtilityComparer's continuation bonus (1.1) is applied to the current behaviour's utility; the prey tracker's FloatTweenBasic smoothing is a lerp of 0.15 a tick.
- The young screams and dies when anything holds it (no Saint exception).
- Eating: a slugcat can pick up a young one handed, carry it through pipes, toss it, and eat it in 5 bites for 2 pips (items.cpp/shortcuts.cpp wired like the baby centipede); the Saint is stunned instead. No Gourmand/Spearmaster specifics beyond what items.cpp already does.
- Graphics: the CicadaWing shader isn't ported: the CentipedeWing quads use vanilla's vertex colours with alpha (0.5 tips, 0.8 roots). DrawSprites' Random roll for the screaming body's swelling is a hash. The MudOverlay (IMuddableGraphics) and culling aren't ported.

## Pole plant (PoleMimic)

- No AI in vanilla either (CreatureTemplate.AI false); safari control (InputCircularRegion), Downpour's LickedByPlayer and the dev tools' "b" pull are not ported.
- Initiate's walk along the line is the story session's 100 tiles; the spawn data's own number (an integer in the world file's creature flags) isn't read.
- A pole plant made away from a pipe (`--creature PoleMimic,x,y`, the fuzz/bench spawns) roots in the terrain closest to where it is made (below first, then left, right, up, within 10 tiles), the stalk standing away from it; vanilla only places one at a den node. Such a plant has no den: withdrawing (hurt or dead) it is destroyed (vanilla's non-den case), and prey it pulls to its root stays pinned there instead of being dragged into a pipe.
- World spawns: the plant is laid out as a pole (vanilla's PlaceInRoom/Initiate, mimicking) the first time its den lets it out; spawns.cpp sets its den wait to 0 (vanilla's remainInDenCounter = 0), but it still travels the den pipe for a moment, so it appears a second or so after the world starts instead of standing there from the start. Later trips out of its den are SpitOutOfShortCut (awake, not mimicking), as vanilla.
- Dragging prey into the den goes through dens.hpp's after_update (going_home with the den entrance as the travel tile): the prey is eaten by the shared den code. A dead plant that has withdrawn is destroyed (ShortcutHandler's MMF behaviour for a dead pole plant) rather than squeezing into the pipe.
- Player.GraspWiggle is a Remix option: a held or stuck slugcat can't wiggle free (escape by hurting the plant: any hit makes it let go, PoleMimic.Stun).
- HitByWeapon's anger is applied to stab and blunt damage (the ported weapons), not explosions or shocks.
- Projectiles hit the stalk (PoleMimic's appendage: root, then the tentacle chunks) through a pole_mimic::trace_stalk hook in creature::trace; the hit pushes the two tentacle chunks around it (ApplyForceOnAppendage).
- Climbing: the stalk is a vine of rot::vines (ClimbableVinesSystem), with the player's vine position carrying the plant (`player.vine_mimic`); VineBeingClimbedOn and VineCurrentlyClimbable are called as in Player.cs. Zero gravity vine grabbing isn't ported (as for the rot vines).
- The "huntChunk in another collision layer" manual Collide only covers leeches (the only other collision layer here).
- Sounds: Pole_Mimic_Stick, Pole_Mimic_Unstick, Pole_Mimic_Grab_Player, Pole_Mimic_Grab_Creature, Player_Grab_Pole_Mimic and Pole_Mimic_Movement_LOOP have no sound ids in sound_types.hpp: skipped.
- Graphics: the shadow mesh (sprite 1, in the Shadows container) isn't drawn (the renderer has no shadow layer). The reveal's "first reveals" (PoleMimicEverSeen in the save) is kept per plant, not per save file. The rolls of the leaves' cosmetics use the creature's graphics_random; the leaves' ruffle when a chunk sticks only happens once a camera has made the graphics module (vanilla's module always exists).
- The tentacle's Visual and Rope.Update use the room's terrain proximity map to skip ray traces that can't hit terrain (same results, as daddy_tentacle.cpp).

## Monster kelp (TentaclePlant)

- The tentacle is kept with the plant (data object properties), not as a creature_part: there is always exactly one, with no stun or health of its own.
- Tentacle.cs is mirrored from daddy_tentacle.cpp's port (same shortcuts: the terrain-proximity fast paths for Visual and Rope.Update, the heap-ordered FindTentaclePath), with the plant's TentacleProps (rope, shorten, goal attraction 0.5/0.05, align 0.05, backtrack 2.2, velocity cap 12).
- AI: one shared tracker with up to ten representations instead of vanilla's single one. Prey and threat modules are capped at one, as in vanilla. Anything unseen for 30 ticks is forgotten, so a creature that keeps still is lost as in vanilla. Vision is movement-based from a random tentacle chunk (visualRadius 400, movementBasedVision 1). The water check is an approximation: being submerged on one side and not the other costs a full point.
- TentaclePlantAI's item interest is not ported: the plant does not reach for moving or thrown items and never grabs items, only creatures. preyInterest only mattered against itemInterest, so it is gone too.
- Remix (MMF) behaviour is used: any Violence makes it let go. A hurt plant may let go of a non-slugcat (2%/tick). Player.GraspWiggle is treated as off, so a held slugcat can't wiggle free; it needs to hit or stun the plant.
- Safari control, LickedByPlayer (Saint's tongue), Yellow's (Monk's) easier grab and devtools input are left out.
- Going home uses dens.cpp: the plant sets going_home/travel_tile to its den entrance and dens::after_update pulls it in, eating what it holds on the way in. remainInDenCounter is set to 1200 after a catch and 400 after a threat. The vanilla 160 for items does not apply because it never holds items.
- A plant rooted somewhere other than a den entrance (the --creature CLI, the selftest spawns) treats the solid tile next to it as its den entrance: floor first, then walls, then ceiling, else the floor below. It eats a catch in place (killed, a slugcat marked eaten, a creature destroyed). After a threat it stays drawn in for 400 ticks instead of going into a den. Dead or drawn in while hurt, it is destroyed, like vanilla's non-den case. A dead plant with a den also vanishes once drawn in, instead of going into the den as a corpse (corpses can't enter dens here).
- No rain behaviour: like vanilla's TentaclePlantAI, it doesn't hide from the rain.
- World spawns: spawns.cpp puts it in its den for 100–1000 ticks like every kind. Vanilla's AbstractCreature sets remainInDenCounter = 0 for TentaclePlant, so vanilla plants come out at once.
- No sounds: Tentacle_Plant_Move_LOOP, Tentacle_Plant_Shake_LOOP, Tentacle_Plant_Init_Thrust and Tentacle_Plant_Grab_Slugcat/Other have no sound ids in sound_types.hpp.
- Weapons hit the tentacle as an appendage via creature::trace (a crossing of the tentacle's chunk-to-chunk segments). The hit goes to the tip chunk as momentum instead of through ApplyForceOnAppendage along the tentacle. Spears (damage type stab) get the +50 stun bonus.
- Graphics: TentaclePlantGraphics is ported (the 40-segment rope tube, 50 danglers seeded from the creature's seed with UnityEngine.Random's algorithm, their physics and draw order). Divergences: the JaggedSquare and TentaclePlant shaders are drawn as flat colours, and sprite alphas are ignored. The region's kelpColor and the palette-13 colour variant aren't available, so the default formula is always used. Room.Darkness(rootPos) is approximated by the palette's darkness. No culling. The dangler layout is built the first time a camera looks at the plant.

## Jetfish

- No Remix (MMF) branches: the old swim speeds (Swim(1), 1.8 base swim speed, 1.2 for a heavy rider), no personality energy, no GoThroughFloors/terrainSqueeze changes while ridden, no gift (DangleFruit offering) handling. The MoreSlugcats 0.9 rider damping for bodyWeightFac >= 1.3 is kept.
- JetFishAI simplified: no StuckTracker (no GetUnstuck behaviour, no stuck shaking, no squeezing into a nearby shortcut when stuck), no GoToFood (fruit: DangleFruit, SwollenWaterNut, LillyPuck, GlowWeed never meet creatures here), no froot returned to the den, no NPCTransportation, no safari control. Idle trips to other rooms (migrate.hpp) are taken only when the exit is reachable by swimming.
- UtilityComparer has no smoothing (vanilla has none for these modules either); ties pick threat, then prey, then rain.
- SocialMemory keeps 4 creatures (the slugcats it met); a new one replaces the weakest like. No community influence (CreatureCommunities) and no InfluenceLike/TempLike.
- PathFinder: a flow field to the destination (pathing.hpp, `jetfish::swim`: water tiles only) instead of FishPather; CoordinateReachableAndGetbackable is a field from the fish's own tile. FishPather's OutOfElement isn't ported.
- It never drowns nor suffocates out of the water (vanilla has neither for JetFish): it just flops and hops on land.
- No sounds: Jet_Fish_Water_Jet_LOOP, On_Land_Jump, Ram_Creature, Grab_NPC/Grab_Player, Light/Heavy_Terrain_Impact are missing from sound_types.hpp.
- No WaterJet particles out of the tentacles while jetting at the surface; the ram's 5 Bubbles are the shared particles::bubbles.
- No eating of a dead jetfish by the slugcat (meatPoints 4): rwcpp has no Player.EatMeatUpdate yet (a generic carcass eating system for every kind would cover it).
- Albino: rolled from the region of the room it is created in (SB or the region's albinos flag), not AbstractCreature's spawner region; no arena 4% albinos.
- The slugcat side shares the cicada's HeavyCarry code: pick up (TwoHands while `grabable`), lay down with pick up, toss with throw, the hands on the fish, the pull 5 + 8.5 apart, following through pipes. Player.DeepSwim/SurfaceSwim skip the slugcat's strokes and set waterFriction 1 while it holds a conscious fish.
- Graphics: culling isn't ported (GraphicsModule.culled); the flipper element sizes (JetFishFlipper0..4 sourcePixelSize) are hard coded from the game's atlas. Faithful otherwise (sprite order, z rotation frames JetFish0..8, eyes, whiskers, tentacles, flippers, albino vertex colours).

## Scavenger

- Only the plain Scavenger template is ported: no Elite, King, Templar or Disciple (no masks, armour, karmic armour, jumps, boomerangs).
- Two grasps instead of vanilla's four: grasps[0] is the throwing hand and grasps[1] the other one (a spear in it is drawn on the back, like vanilla's grasps 1..3). ArrangeInventory works on those two.
- The AI is a cut-down ScavengerAI. It keeps Idle (IdleScore without tile visibility, exit distances, discomfort or the alreadyIdledAt list; it still follows the more dominant scavengers' idle spots), Flee (ThreatTracker.FleeTo, throwing at the threat 25% of ticks), Attack (CheckThrow/TryThrow; the throwing positions are a simplified SpearThrowPositionScore with no flood fills of either creature's movement area), EscapeRain/Injured (dens::home_tile), Scavenge (weapons only) and Travel (migrate.hpp trips).
- Not ported: trading, gifts, squads and squad leaders, tolls and outposts (GuardOutpostModule), communication and pointing animations, rummaging, the NoiseTracker (Investigate), FindPackLeader, the DiscomfortTracker, ghosting out of rooms, the MSC locust and graffiti logic, safari control.
- The relationship to the slugcat replaces the community reputation and SocialMemory with one `like` per scavenger, shared by every slugcat. It drops when a slugcat's thrown weapon hurts that scavenger (-0.5) or any scavenger in its room (-0.25 for the others). It never recovers. With like >= 0 the slugcat is ignored. Below 0 the scavenger Attacks while it holds a weapon and is Afraid without one.
- The violence types are only Warning and Lethal (no NonLethal or ForFun). A hostile slugcat gets about 160 seen ticks of Warning first: the throw charge with its shake, plus vanilla's warning throws aimed over the target. After that, or once the slugcat comes within 200 px, the violence is Lethal. Other threats are always Lethal.
- Who hurt it is found by looking for a slugcat's thrown item next to the hit chunk when Violence runs, because rwcpp's Violence gets no source. Slugcat melee and explosions don't count.
- WeaponScore and CollectScore only score the items rwcpp has (spears 3, explosive spear 4/1, rocks 2, bombs 3/0, flare bombs and lily pucks 1). Pearls, food and the other valuables are never collected, so `bringPearlHome` is gone too. The ItemTracker is replaced by scanning the room's visible free items every 40 to 200 ticks.
- Movement follows pathing.hpp's distance field (the Scavenger's resistances: floors and corridors 1, poles 2.5). Its steps are Standard, OpenDiagonal and DropToFloor only. There are no ReachUp, DoubleReachUp, ReachOverGap or SemiDiagonal reach moves, no in-room shortcuts, NPC transportation or region transportation, and no pathingWithExits. Swinging between poles (Swing) only happens along climbable tiles on the path; there is no jump across gaps.
- Swim mode is ported. With no swim pathing (the movement has no `swim`), a scavenger that ends up in deep water just paddles at the surface towards its destination.
- Not ported: FlyingWeapon dodging and blocking incoming weapons (TakeDownIncomingWeapon), MeleeGetFree when grabbed, Collide's push between scavengers (scavenger-on-scavenger collisions go through creature.cpp's collide_kin, which has no scavenger case), the AI's UtilityComparer tweening (utilities are compared raw), the CreatureLooker (it looks at its focus creature, else at its look point), and visionFactor/narrowVision (fixed at 0.75 and 1).
- FastReactionCheck is approximated (`reflexBuildUp >= 1 || 50%`). DLCShared's reaction-skill charge times aren't used: charges last 5 ticks before a melee or mid-range throw.
- Throws use vanilla's speed (max(20, exitThrownModeSpeed + 5) = 35 along the aim). The item starts at the first-frame trace point (main - dir*5) rather than at thrownPos (main + dir*50), so targets close by are still hit by rwcpp's per-frame trace. thrownPos is still main + dir*50.
- The migrate roll (AbstractBehavior) is made on 0.2% of ticks, not every tick, because migrate.cpp has no Scavenger laziness entry (vanilla's abstractedLaziness is 50) and the shared table was left alone.
- Scavengers use the room's creature dens through dens.hpp like the other kinds (vanilla scavengers don't use creature holes). They drop what they hold when they go into a den. Items they hold through a room-to-room pipe come along when they come out.
- Sounds: none of the Scavenger_* sound ids exist, so the knuckle, grab-pole and grab-terrain sounds are silent. Picking up uses Slugcat_Pick_Up_Spear (as vanilla does) and throws use the slugcat throw sounds.
- Graphics: ScavengerGraphics is ported with its rolls from the seed, in vanilla order: IndividualVariations, GenerateColors, the back cosmetic (HardBackSpikes or WobblyBackTufts with their patterns and scales), the eartlers and the teeth. The body, waist, chest patch, neck, head, eyes, pupils, teeth, eartlers, arms with ScavengerHandA/B, legs and tail are all drawn. Differences:
  - The darkness isn't lowered by light sources (LightSourceExposure).
  - No mud overlay, and no blindness tint on the eyes.
  - HardBackSpikes' per-frame bristle shiver (a DrawSprites random) is left out.
  - The TailSegments' PushOutOfTerrain ignores the base point.
  - The leg's and hand's unused "pixel" sprites aren't drawn.
  - Held items aren't put in the internal sprite containers. They draw where rwcpp's item pass draws them, not between the scavenger's sprites.
  - Where no camera looks, held items sit on the main chunk (vanilla without a graphics module).
- Personality is rolled here from the seed (AbstractCreature.Personality) and kept in the data object. It feeds the combat skills, the colours, the eartler size and the AI.

## Rain deer

- No Remix (MMF) deer behaviour: the vanilla (non-MMF) branches throughout (legs 220 long, antlers Lerp(30, 70, dominance), every-tick wander rolls, no wiggle-to-startle, no deer-control tutorial).
- Deer-to-deer social behaviour left out: no bowing/kneeling to more dominant deer (`Kneeling` is never on), no deer piles (deerPileCounter), no dominance checks for puff balls or rest spots, no TravelPreference resistance near other deer. Deer still push each other through the generic same-kind collision, antlers included (vanilla's antlers don't collide with objects).
- Collisions with the other creatures are the deer's own pass (deer.cpp `collide_others`, body chunks 0-4 only, so the antlers stay out of it); the other kinds' Collide callbacks aren't called for deer contacts.
- Room changes: vanilla deer travel off screen through the side exits (DeerAbstractAI's allowed rooms and nodes). Here a deer never roams between rooms on its own; it goes home through the pipes (dens.hpp) when the rain comes or when hurt (damageGoHome), and travels the pipes (migrate.hpp trip) when a spore cloud in another room of its region calls it.
- Pathing: one distance field (pathing.hpp, Air allowed) with `open_air` 3 standing in for TravelPreference's `|preferredHeight - terrainProximity|` cost; smoothedFloorAltitude is the plain floor altitude; CoordinateReachableAndGetbackable is "reachable from the destination's field". DeerPather's look-ahead along the path is kept.
- StuckTracker: past positions only; GetUnstuckPosCalculator replaced by a random nearby destination.
- ItemTracker: the deer knows the free puff balls of its room it sees (1450 px, line of sight) and remembers where it last saw the one it goes for. Held puff balls are never targets (as in vanilla without Remix), so heldPuffballNotGiven / deniedPuffballs are left out.
- SporePuffVisionObscurer.AttractADeer runs at the burst instead of ~39 ticks later; the cross-room summons picks any deer of the region (not only vanilla's allowed deer rooms) with a trip there, half the time even when a deer of the room was called (like vanilla).
- The CreatureLooker/Tracker isn't ported: the eyes look at the puff ball it goes for, else the closest slugcat in view within 600 px, else the spore spot.
- PlayerInAntlers: no vulture repelling, no Remix wiggle; riding needs the deer's room to be viewed (vanilla needs its graphics module). The rider's sprites aren't drawn inside the deer's containers (between the far and near horn danglers): the slugcat draws on its own layer. PlayerGraphics' AntlerClimb leg pose (num3 = 2) isn't ported; the hands are (player_graphics.cpp).
- Sounds without a sample here are skipped: Leviathan terrain impacts, Vulture_Tentacle_Grab_Terrain (leg steps), Puffball_Eaten_By_Deer, In_Room_Deer_Summoned, Slugcat_Climb_Along_Horizontal_Beam (jumping off the antlers).
- Graphics: the body chunks' JaggedCircle shader is drawn as plain Circle20 ellipses (no ragged rim); everything else follows DeerGraphics.DrawSprites in sprite order.
- Spore sickness wobble (InsectoidCreature.poison and the deer's own) isn't ported.

## Puff ball

- SporeCloud is cosmetic dust puffs of particles.hpp (glowing circles in the spore colour, an approximation of the Spores shader sprite), only where a camera looks; their colour is fixed (the palette's texel(11, 4) is taken as the base colour) since the simulation has no palette.
- The clouds' toxicity is one stun at the burst for the insect kinds ported (centipedes, dropbugs, cicadas, noodleflies) within 90 px, ~60 ticks over their mass; no InsectoidCreature.poison, no flies/spiders/locusts/small insects, no kill tag, no repelLocusts.
- PuffBallSkin shreds, SporesSmoke from the stalk, SporePuffVisionObscurer's vision blocking, the slugcat swallowing it (`swallowed`) aren't ported; HitByWeapon/HitByExplosion bursts aren't either.
- Exploding on a creature doesn't give it PuffBall.HitSomething's push (vel * 0.1 / mass); thrown, its override exit speed uses the generic Weapon cap (30) instead of PuffBall's 15 for weak throws.
- No puff ball sounds (pick up, throw, burst have no samples here).

## Miros bird

- Solo birds. MirosBirdAbstractAI's flocks, which move birds between rooms together, are not ported. A bird roams its own room's floors and changes rooms only through migrate.hpp's trips and dens.hpp's pipes, like every other creature. Vanilla birds leave rooms over the room's edge (side highways, `forbidStandardShortcutEntry`); `enterRoomHalf`'s push out over the edge is not ported either.
- Idle is a random roam (not vanilla). An idle bird walks to a random reachable floor spot with open space above it, lingers there, and then picks another (`idle_pos`/`idle_counter`). Vanilla's idle bird heads for its den and lets the flock's abstract AI move it about.
- Simplified AI. It keeps MirosBirdAI's utility comparison of threat (0.9), prey (0.5) and rain (0.9), with no smoothing or continuation bonus. Flee goes through ThreatTracker.FleeTo, Hunt runs at the prey's best-guess tile, EscapeRain and ReturnPrey head for the den. GetUnstuck (StuckTracker and its unstuck position) is left out, and the body's stuck shake is what frees it.
- CreatureLooker is reduced to looking at the focus creature: the prey while hunting, the threat while fleeing, as long as it is still tracked. With no focus it looks ahead along its move direction. Vanilla's interest-based switching between tracked creatures is gone.
- Pathing runs over the floor tiles (the `walker` movement: floors plus drops), starting from the floor tile under the body, and the body is steered at points 80 px (`body_lift`) above the path. Vanilla paths through Air, and TravelPreference keeps the path about five tiles above the floor. FollowPath's fallbacks (offset start tiles, picking the most accessible neighbour) are reduced to trying the floor under the other chunks. Low tunnels count as floor, so a bird may try to walk into spaces too small for its body.
- "At the destination" also counts when the floor tile under the body is within 1 tile of the destination, or within 3 tiles when the bird is heading for a pipe. When heading for a pipe, the body is aimed at the pipe tile itself, so it bends down into floor-level entrances that the shared den code pulls it into.
- The beak cycle is ported as in vanilla. The jaw opens and slams shut on its own, and a slam bites whatever it wants to bite in front of the beak (not miros birds, not small creatures). The bite is Grab plus Violence(1.2, Bite), which kills a slugcat. Grab shareability isn't checked. Biting a daddy's tentacles doesn't stun them.
- No jump of its own. Vanilla MirosBird has none either: its leaps come from the run-mode leg springs, which are ported.
- No sounds. The Miros_* sound ids (piston impacts and scrapes, beak snaps, terrain impacts) are not in sound_types.hpp. The legs' InGameNoises are not ported.
- Spark particles from the feet use the shared particle kind, with approximate lifetimes, and only in viewed rooms. They roll from graphics_random.
- The neck is vanilla's tile Tentacle without a rope (the neck's tProps.rope is off anyway). Its segment and grab-path lists are capped at 64 tiles each.
- Graphics: the feet's LightSources (the spark glow in dark rooms) are not ported. Room.Darkness for the eye trail is the palette's darkness, ignoring light sources. Blinded eye flicker and culling are not ported.
- Safari control, the dev-tool keys and `iVars.fatness` (unused by the graphics) are left out.

## Worm grass

- Creatures don't avoid worm grass: `WormGrass.IsTileAccessible` (IAccessibilityModifier) and the column of `Tile.wormGrass` that `SortTiles` marks over each patch tile aren't ported, so pathing walks creatures into patches. The deer's `DeerAI.wormGrass` isn't ported either.
- Cosmetic worms: vanilla makes them only for the tiles near a camera and remakes them each time the camera moves (`NewCameraPos`, with an unseeded `System.Random`). Here every cosmetic worm is made once with the patch and kept, and it only updates while a camera looks at the room. Per-worm culling (`ViewedByAnyCamera(pos, 200)`) is "a camera looks at the room".
- The patch layout is rolled from `rw::rng`, seeded by LeftTile.x + LeftTile.y + our room index, not from vanilla's `System.Random` with the AbstractRoom index. The patches come out the same size and shape, but the worms' exact jitter, lengths and colours differ from the game.
- `BodyChunk.Rotation` is taken as (0, 1) (our physics has no rotation chunks), so a worm's hold on a chunk doesn't turn as the chunk turns.
- `Player.GraspWiggle` (Remix's grasp wiggling) isn't ported: a slugcat can't wiggle a worm off. It only gets free by pulling far enough away, or when an explosion knocks the worm off.
- `WormGrass.repulsiveObjects` (only the MSC Gooieduck adds them) and `WormGrassGooduckyImmune` aren't ported. Neither are the MSC/Watcher bits: void weaver pacifying, the OE glow light and tint, Saint's random LF patches (`Room.Loaded`), and the ripple layer checks.
- Immunity: `wormGrassImmune` covers only the templates that are ported (green lizard, spit lizard, monster kelp, pole plant). `tentacleImmune` is only the world file's TentacleImmune tag (rot lizards aren't ported).
- The "climbing out" push for conscious non-slugcats tests `pathing::accessible` with `standard_ground` up the column, not the creature's own template's AImap.
- `trackedCreatures` holds at most 8 creatures per patch (vanilla has no limit).
- Eating: when the creature dies it gets `killed_by_other`. Destroy is the rot's path: any creature but a slugcat is destroyed at the end of the tick. A slugcat stays dead and `eaten` (Remix's `PermaDie` / co-op details aside). `abstractCreature.Move` to node -1 isn't done.
- Explosions use our `explosions::desc` radius and force, which may not match vanilla's `Explosion.rad` and `force` for the same weapon.
- ApplyPalette's `RoomCamera.PixelColorAtCoordinate(belowGroundPos)` (the level image under the worm's root) isn't sampled. The palette's play-layer terrain colour (texel 0, 3) stands in for it.
- No sounds (vanilla's worm grass plays none).
