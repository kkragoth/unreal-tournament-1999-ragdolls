# Handoff (2026-10-11, evening) - spawn pose from the death animation

User feedback (via `docs/handoff-2026-10-11-damage.md`, "Next"): "the pawn
has a frozen pose, then the ragdoll spawns in a different position". The
ragdoll now spawns in the dying pawn's CURRENT animation frame, fitted
natively from the pawn mesh; the random pose (`PoseVariation`) is the
fallback. Built, deployed to BOTH installs (DLL 12:48, .u 12:48 on D:\old /
12:59 on the dev copy, see "Deploy state"), verified headless. Not yet
playtested in the client, not committed (the morning's pivot-hip and damage
work is still uncommitted too).

## What the handoff plan assumed, and what was actually true

The plan was "take the dataset vertices dominantly bound to each body and
fit the rigid transform onto the pawn mesh's frame vertices". That needs a
vertex correspondence between the dataset and the live mesh, and there was
none: the datasets were derived from the Selection viewer meshes
(SelectionMale1...), the pawns use Commando/Soldier/Boss/SGirl/FCommando.
Offline comparison (`tools/extract_mesh.py` on Botpack.u):

- same vertex count (Commando 332 incl. 3 special, SelectionMale1 332), same
  face count (642), same connected pieces (297 + 16 + 8 + 8 verts), same
  edge count, so they are the same model re-exported;
- DIFFERENT vertex numbering and UV wedges (0 of 642 faces equal as vertex
  triples), so dataset vertex k is not live vertex k;
- the dataset's first N vertices ARE the Selection mesh's visible vertices
  in order (pairwise-distance test, median error 0.00 UU; the rest are the
  subdivision midpoints).

So the correspondence is a permutation, recovered from connectivity alone.

## What changed

### `tools/build_live_map.py` (new) -> `RagdollPkg/Src/RagdollLiveMaps.h` (generated)

Matches each live mesh onto its Selection twin piece by piece: once one
oriented face is matched, the third vertex of the face across every matched
directed edge must match too, so a seed face propagates over a whole piece
(`propagate`, with re-seeding across the two non-manifold edges of the
Boss). Pieces with a unique topological match are accepted first; the
dataset's left/right bone labels check the main body's facing, and the
remaining ambiguous pieces (the two identical shoulder pads, the Boss's
nine armour plates) are resolved by predicting where each candidate's
dataset verts land under the per-body rigid fit of what is already mapped.
A per-body Horn fit of the live `Dead1` frame 0 verifies the result:

| model | pieces | per-body rms on Dead1+0 (UU) |
|---|---|---|
| Commando | 297+16+8+8 | 0.5 .. 2.4 |
| Soldier | 288+9+9 | 1.0 .. 2.6 |
| Boss | 50+50+50+36+32+26+26+20+20 | 0.4 .. 3.0 |
| SGirl | 290+9+9 | 0.6 .. 2.9 |
| FCommando | 256+38+38+12+4 | 0.5 .. 2.9 |

A wrong mapping scatters those to 10+ UU. Rerun after any dataset
regeneration (it reads `RagdollDatasets.h`):
`python tools/build_live_map.py --header RagdollPkg/Src/RagdollLiveMaps.h`
(no numpy needed; ~1 s). `FRagdollLiveMap` / `FindRagdollLiveMap` live in
`RagdollInternal.h` / `RagdollDatasets.cpp`.

**Finding: the Boss dataset faces backwards.** The fit of the live Boss
puts the dataset's "front" (+Y) at 174 deg from the pawn's forward and its
"UpArmL" verts on the pawn's right. `_pick_facing` chose the wrong yaw for
SelectionBoss, so until now Boss corpses spawned facing backwards and
their shoulder cones (out+forward in bind space) aim out+BACKWARD. The
animation fit places the Boss correctly regardless (it fits the real
rotation), but the mirrored labels remain: regenerate the Boss dataset with
the facing flipped, then rerun `build_live_map.py` (its facing line should
read ~0 deg for every model).

### Native (`RagdollCarcass.cpp`)

- `ApplyAnimationPose` (runs before the random `ApplyDeathPose`, which is
  now only the fallback): reads the carcass's `AnimSequence` / `AnimFrame`
  (copied from the pawn by `Initfor`; the engine `Carcass.Initfor` is EMPTY
  so nothing copied them before), picks the two keyframes the renderer would
  blend (`StartFrame + AnimFrame*NumFrames`, clamped inside the sequence; a
  negative AnimFrame = tween into frame 0), loads the live mesh's lazy
  vertex array (`Verts.Load()`, needed on a server that never rendered the
  mesh) and transforms the blended vertices to world space with exactly the
  renderer's chain (`UnitCoords * (Location+PrePivot) * Rotation *
  RotOrigin * FScale(Scale*DrawScale)` on `V - Origin`). Every live vertex
  maps through `RagdollLiveMaps.h` to its dataset vertex, whose bone binds
  (all slots with weight >= 0.25) feed a weighted rigid fit per body
  (`FRagdollRigidFit`: Horn's unit-quaternion method, 4x4 Jacobi, residual
  from the largest eigenvalue). Bodies with < 4 vertices or rms > 8 UU x
  DrawScale follow their parent; the pelvis must fit or the whole thing
  falls back.
- The fitted WORLD rotations become RELATIVE rotations down the chain
  (against each parent's effective rotation) and `RagdollPoseFK` (new,
  shared with `ApplyDeathPose`) places the bodies from the fitted pelvis,
  so the joint frames coincide exactly and nothing pops on the first step.
  Elbows/knees keep only the component about their hinge axis (bind +X),
  clamped to the joint range (so the elbow census reads 126-127 on a
  straight-armed frame: the clamp). A ball/pivot joint more than 150 deg
  from bind is a mis-fit and follows its parent.
- Log: `RagdollPkg: animpose <corpse> seq=Dead2 frame=0.15 (540+0.40)
  rms=1.6 UU bodies=12/12 pelvis=(x,y,z) loc=(x,y,z) facing=22 pawnyaw=0
  fails= none` (first 12 corpses). `facing` is the pelvis front's world
  yaw in degrees (pawn yaw 0 = 0; a body on its back reads anything).
  Skips log `animpose <corpse> skipped: <reason>` (no map / no sequence /
  pelvis fit failed) and the random pose runs.
- Knob: `bAnimPose` (globalconfig bool on `RagdollCarcass`, default True;
  `set RagdollPkg.RagdollCarcass bAnimPose 0` restores the random pose).

### Script

- `RagdollCarcass.Initfor`: `AnimSequence = Other.AnimSequence; AnimFrame =
  Other.AnimFrame; AnimRate = 0; TweenRate = 0;` before `BuildRagdoll`
  (the pawn is ~0.2 s into its Dead* animation: Botpack `Dying` does
  `Sleep(0.2); SpawnCarcass()`).
- `RagdollTestMutator`: the direct Commando corpse gets a real death
  sequence per sweep (`DeadAnimForSweep`: Dead1/2/3/7/8) and `AnimFrame`
  0.05..0.25, so the headless log exercises the fit. The spread-sample
  corpses keep the random pose (their `legspread` census is unchanged).
  Test corpses that never had Initfor carry the Engine default
  `AnimSequence='Dead'`, which no retail mesh has -> logged skip, random
  pose, as before.

## Headless measurements (dev install, `ucc server` 125 s, DM-Deck16][)

```
animpose RagdollCarcass3 seq=Dead2 frame=0.15 (540+0.40) rms=1.6 UU bodies=12/12 pelvis=(-104,1002,-669) loc=(-102,1004,-661) facing=22 pawnyaw=0 fails= none
animpose RagdollCarcass4 seq=Dead3 frame=0.25 (557+0.25) rms=1.2 UU bodies=12/12 pelvis=(-106,1002,-677) ... facing=10
animpose RagdollCarcass6 seq=Dead7 frame=0.05 (584+0.05) rms=1.2 UU bodies=12/12 ... facing=13
animpose RagdollCarcass7 seq=Dead8 frame=0.15 (606+0.70) rms=1.5 UU bodies=12/12 ... facing=13
animpose RagdollCarcass8 seq=Dead1 frame=0.25 (528+0.25) rms=1.3 UU bodies=12/12 ... facing=-158
```

Every body fits at 1.2-1.6 UU (the mesh is that rigid per segment, even
mid-fall), no body ever fell back, the pelvis sits 2-16 UU from the actor
location (the death animations move the hips), the sync census afterwards
shows the posed joints inside their ranges with the usual soft-limit
overshoot (`elbowR:127[-3,126]!`, `kneeR:-94`). No crash in 125 s.

Earlier in the session UCC.exe produced two WER crash events (12:47:25 and
12:49:10, both ~1.5 s after process start, ntdll, same bucket). They
predate the DLL even being bound (`Bound to RagdollPkg.dll` comes ~10 s
in) and every logged run with this build was clean, so they look like
overlapping server starts, not this change. Watch for it anyway.

## Deploy state

- D:\old (the playtest copy): `RagdollPkg.dll` 12:48 + `RagdollPkg.u` 12:48
  (has `bAnimPose` and the Initfor copy, i.e. the feature; it predates only
  the test-mutator change). The game was running from D:\old at 12:51
  while the newer .u (12:59) was built, so that .u could not be copied; do
  it after the game exits (dev install `System\RagdollPkg.u` -> D:\old).
- Dev install: DLL 12:48, .u 12:59 (current sources).

## Playtest checklist

- Kill a bot and watch the first frame of the corpse: it should continue
  the frozen death pose, not jump to arms-forward. Log `RagdollPkg:
  animpose ... rms=` should be ~1-3 UU with `fails= none`; `bodies=12/12`.
- `facing` vs `pawnyaw` within ~30 deg on upright frames.
- If a corpse ever spawns with a limb folded wrong, check the `fails=` list
  (a body listed there kept its parent's rotation) and the sequence/frame;
  `set RagdollPkg.RagdollCarcass bAnimPose 0` compares with the old pose.
- Boss corpses: expect the arms to behave oddly (cones aim backward, see
  the finding above) until the Boss dataset is regenerated.

## Next

- Regenerate the SelectionBoss dataset facing forward (fix `_pick_facing`
  for the Boss or force the yaw), rerun `build_live_map.py`, check the
  `facing` line reads ~0 for all five.
- The tween-in case (`AnimFrame < 0`, within the first 0.1 s of PlayDying)
  uses the sequence's first frame; the previous pose is not recoverable
  from the carcass. Rare: the carcass spawns at 0.2 s.
- With the pose now coming from the animation, `PoseVariation` only matters
  for the fallback; the random tumble / killing-blow kick
  (`ApplyDeathVelocities`) still runs on top and is now the only source of
  per-corpse variation besides the shot.
