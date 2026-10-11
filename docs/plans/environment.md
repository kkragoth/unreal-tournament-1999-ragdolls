# Environment interaction: zones, movers, crushing

Plan + implementation notes, 2026-10-11 (evening). Headless-verified on
DM-Deck16][ (see "Status"), NOT yet playtested in the client.

## What the engine does to a corpse, and what the ragdoll did before

| Environment feature | Stock UT (Engine.u) | Ragdoll before | Ragdoll now |
|---|---|---|---|
| Low / high gravity zone (`ZoneInfo.ZoneGravity`) | `physFalling` uses the actor's zone gravity | one scale per corpse, read ONCE at spawn from the pawn's zone (`ZoneGravityUU`) | every body reads its own zone every tick (`b3Body_SetGravityScale`); a corpse falling from a low-grav ledge into normal gravity speeds up in the air |
| Water (`bWaterZone`) | PHYS_Falling actors sink with `Buoyancy`/`Mass`, fluid friction | nothing: corpses dropped through water at full gravity | per-body buoyancy from the submerged fraction (3 samples along Z), water drag (linear + angular damping), a splash + `EntrySound` on entry; corpses float up slowly (`WaterBuoyancy` 1.08) and bob at the surface |
| Water current / wind (`ZoneVelocity`) | swimming pawns, projectiles with `bMoveProjectiles` | nothing | bodies are dragged toward the zone velocity (`WaterDrag` x submerged fraction in water, `AirDrag` in air; `ZoneWindScale` 0 turns the air part off) |
| Lava / slime (`bDestructive`, "destroys carcasses") | the engine destroys carcasses on entry | nothing: corpses sat in the lava for ever | the corpse disappears (zone `EntryActor` splash + sound first); a torn-off limb that drifts in alone is removed on its own |
| Kill zones (`bKillZone`) | kills pawns | nothing | corpse despawns (1 s) |
| Movers: doors, lifts, trains (`Mover` brushes) | actors based on the mover ride it; the mover's `EncroachingOn` gives carcasses `TakeDamage(10000, 'Crushed')` | the Box3D world only held the static BSP: corpses fell THROUGH lifts and doors swung through them | every mover brush is a KINEMATIC Box3D body (one thin convex slab per brush face) driven to the mover's pose every step (`b3Body_SetTargetTransform`), so lifts carry corpses up and down and doors / trains push them |
| Crushed by a mover | `'Crushed'` damage on carcasses = ChunkUp (gib) | 10000 damage = total gib (bloody) | the corpse just disappears: a body centre inside a mover brush (engine `UModel::PointCheck` against the mover, checked every 4th sync near moving movers AND on the `'Crushed'` damage) despawns the corpse; a crushed detached limb is removed alone |
| Fell out of the world | `FellOutOfWorld` destroys | despawn below the lowest BSP vertex | unchanged |
| Teleporters, kickers (jump pads), warp zones, `bBounceVelocity` zones | act on pawns / `bCanTeleport` actors only | nothing | still nothing (not in scope; teleporting a 12-body corpse needs a world-space rebase) |

## Design

### Movers (`UnRagdollGeometry.cpp`)

- `FRagdollLevelWorld::Movers` (`FRagdollMoverBody`: `AMover*`, kinematic
  `b3BodyId`, last `Location`/`Rotation`, `LastMoveTime`). Built with the
  level world: every `AMover` in `Level->Actors` with a `Brush`.
- Geometry: the brush's own polygons (`Brush->Polys` when present, else its
  BSP `Nodes`/`Points`), transformed to world with the engine's
  `ABrush::ToWorld()` (bakes `PrePivot`, `MainScale`, `PostScale`), then into
  the mover's rigid frame (`Location`, `Rotation` as a quaternion from
  `GMath.UnitCoords / Rotation`). Each face becomes a convex slab: the face
  polygon plus a copy pushed 12 UU INTO the brush (`b3CreateHull` ->
  `b3CreateHullShape`). Slabs instead of one hull per brush so non-convex
  movers (train cars, L-shaped lifts) keep their interior; instead of a mesh
  shape because Box3D mesh shapes only collide on STATIC bodies.
- Category `RAGDOLL_GEOM_CATEGORY` (the clearance ray and the under-floor
  probe treat movers as floor), friction 0.6, `userData` NULL (the contact
  census counts them as geometry; `b3Body_GetType` tells a mover apart).
- Every world step (`RagdollSyncMovers`, called from `StepRagdollImpl` right
  before `b3World_Step`): a mover whose pose changed gets
  `b3Body_SetTargetTransform(pose, dt)` (so its contacts carry the right
  surface velocity: friction rides the corpse along) and every corpse body it
  touches is woken (a sleeping corpse on a lift that starts moving would
  otherwise hang in the air); an unchanged mover gets zero velocity and is
  snapped back onto its pose if it drifted. Destroyed movers are disabled.
- Validation at build: the transformed vertex bounds are compared with the
  engine's `Brush->GetCollisionBoundingBox(Mover)` and the rotator round trip
  (`OrthoRotation` of the axes) with the mover's `Rotation`; mismatches are
  logged (`RagdollPkg: mover ... WARN`).

### Zones (`RagdollCarcass.cpp`, `RagdollApplyZones`)

Runs for every corpse at the top of `StepRagdollImpl` (before the shared
world step; forces accumulate until the next `b3World_Step`). Per body
(skeleton + fragments):

- Zone of the body centre: `Level->Model->PointRegion(LevelInfo, P)`.
- Gravity: `b3Body_SetGravityScale(GravityScale x (-ZoneGravity.Z / 950))`
  whenever it changes; a sideways gravity component becomes a force.
- Water: submerged fraction `f` from three samples along world Z (centre and
  +-half extent). Buoyancy force `f x m x g x WaterBuoyancy` (1.08: bodies
  float up at ~0.5 m/s^2 net), linear damping `0.05 + WaterDrag x f`
  (WaterDrag 2.0 /s), angular damping `AngularDamping + WaterDrag x f`,
  current force `f x m x WaterDrag x ZoneVelocity`. On entry the body's
  velocity is halved (the splash). Forces wake a body only when its zone
  state changed, so a floating corpse can fall asleep at the surface.
- Air with `ZoneVelocity` (wind tunnels): force `m x AirDrag x
  ZoneWindScale x ZoneVelocity` plus linear damping `AirDrag` (0.5 /s).
- `bDestructive` / `bKillZone`: if the body's island root is the pelvis the
  corpse is hidden and given `LifeSpan` 0.01 (destructive) / 1 s (kill);
  a detached island is removed natively (`RagdollRemoveIsland`). The script
  `ZoneChange` (actor = pelvis) adds the zone's `EntryActor` / `EntrySound`
  and destroys immediately in destructive zones.
- Knobs (`RagdollCarcass` globalconfig, read at `BuildRagdoll`):
  `WaterBuoyancy` 1.08, `WaterDrag` 2.0, `AirDrag` 0.5, `ZoneWindScale` 1.0,
  `bMoverCollision` true (read when the level world is built: the first
  corpse's value wins until the level changes), `bCrushDestroys` true (false
  = the old gib).

### Crushing

- `RagdollCrushCheck(S, C)`: for each alive body, for each mover whose
  `LastMoveTime` is within 1 s (or every mover when called from damage),
  `Mover->Brush->PointCheck(Hit, Mover, centre, zero extent)` returns 0
  (hit) when the centre is inside the brush. Main-island body inside ->
  `bCrushed`: the corpse is hidden and expires next tick (no gib, no blood);
  a detached island inside -> removed alone. Called every 4th sync when the
  level has movers, and from script on `'Crushed'` damage
  (`RagdollCrushed()` native, returns 1 when the main island was crushed so
  script can `Destroy()` at once).
- Why not the engine's encroachment alone: the mover's encroachment test
  uses the corpse's collision CYLINDER (which covers the whole sprawled
  body), so a door closing next to a corpse would delete it; the per-body
  brush test only fires when a body really is inside the brush.

### Log lines

- `RagdollPkg: movers level=<name> count=N slabs=M` and per mover
  `RagdollPkg: mover <name> faces= slabs= bounds=(...) engineBounds=(...)`.
- `RagdollPkg: zone <corpse> body=i zone=<name> water=f grav=x` on changes
  (first 40 lines per level).
- `RagdollPkg: crushed <corpse> body=i mover=<name>` and
  `RagdollPkg: destructive <corpse> body=i zone=<name>`.
- Script: `RagdollCarcass: ZoneChange <corpse> -> <zone> water= destructive=`.

### Headless test (`RagdollTestMutator`, sweeps 7-12)

- `envtest water`: the first `bWaterZone` zone of the map (Deck16: the
  `SlimeZone` pool; its `bDestructive` is cleared for the test so buoyancy is
  measured, and a second corpse is dropped with it set to check the
  despawn). Logs the pelvis Z and the native `water=` fraction each sweep:
  the corpse must rise toward the surface and settle, not fall to the bottom.
- `envtest mover`: the first `Mover`; a corpse is spawned on its top surface
  (traced from above), the mover is triggered, and the corpse's pelvis Z vs
  the mover's Z is logged each sweep: the corpse must ride.
- `envtest crush`: a corpse spawned with its pelvis INSIDE the mover brush
  gets `TakeDamage(10000, 'Crushed')` and must report crushed/destroyed; a
  control corpse on open floor given the same damage must survive.

## Status (2026-10-11 night, `ucc server` DM-Deck16][, 120 s x 4 runs)

Headless verified, NOT yet playtested in the client.

- Water (`envtest water`, SlimeZone0 with bDestructive cleared): the
  corpse spawned at -1534 first SANK to the pool floor (-1589) in every
  run and sat there. Root cause was not buoyancy at all: the shared world
  was stepped once per CORPSE per tick (appSeconds() dedupe), each extra
  step ~1 ms, and Box3D clears applied forces after every step, so the
  water corpse's forces (ledger `lift=1651 weight=1529`, all 12 bodies
  wet, awake) acted for a millisecond. With one step per engine tick
  (`LastTickTime`) it rises at 0.75 m/s: pelvis -1527 -> -1450 -> -1368,
  top body at the surface (-1334) by sweep 13; then the restored
  bDestructive removes it (`destructive` line) as expected. The lava corpse
  and the kill zone path unchanged.
- Lift (`envtest mover`, Mover0 552 UU in 1.75 s = 315 UU/s): the
  kinematic body used to trail the engine brush by 20-30 UU (catch-up
  spread) so the corpse rode INSIDE the platform; with the velocity lead
  `Vel x max(CATCHUP - dt, 0)` the native `ride` lines read
  `engZ=-1074.7 kinZ=-1075.8` / `-775.5 / -776.5` (within 1 UU) up and
  `-785.4 / -784.7`, `-1088.1 / -1086.7` down. A full lead of
  `Vel x CATCHUP` overshot by 16-22 UU on the 20 Hz server. The script
  sweep lines sample before the tick's step, so their `lowestAboveTop`
  reads -28..-34 while the lift moves (one engine tick of travel); at
  rest it is -0.6..+5 (body centre vs the top plane).
- Crush: the control corpse survives, the corpse spawned inside Mover1
  is destroyed (`hits=2` from the 'Crushed' damage path). The ride corpse
  WAS deleted at the bottom of the ride in run 3: the lift goes down at
  6.3 m/s, the corpse starts from rest, lands ~100 UU later at ~14 m/s and
  a shin dipped 5.5 UU into the 12 UU slab (past its mid-plane: contact
  normal flipped); the non-convex path then called any point inside the
  shell "inside". Now: slabs 16 UU (clamped to the brush depth per face),
  `contactHertz` 60, and a crush needs the centre plus the six +-6 UU axis
  points inside. Run 4: the ride corpse survives the up/stop/down/stop
  cycle, the real crush still fires.
- Gore / damage unchanged by this: ten cuts grow the mesh 1292 -> ~5000
  verts (was 375k), `dmgtest` numbers as in the damage handoff.
