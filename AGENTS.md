# AGENTS.md

Notes for AI agents working in this repo.

## Build/deploy loop (native + script)

- First checkout only: provision gitignored deps with
  `.\setup.ps1 -Ut99Src "<UT99 public source tree>"` (adds `thirdparty\ut99sdk`
  headers/libs and `thirdparty\box3d`; add `-Configure` to configure CMake too).
- Native-only edits (`.cpp`/`.h`): build `build\RagdollPkg\RagdollPkg.vcxproj`
  with MSBuild `/p:Configuration=Release /p:Platform=Win32
  /p:BuildProjectReferences=false`, then copy `System\RagdollPkg.dll` to the
  retail `System\`.
- `.uc` (UnrealScript) edits: **you MUST delete the retail
  `System\RagdollPkg.u` first**, then run `ucc make RagdollPkg`. ucc make skips
  recompilation when the `.u` exists, even if the `.uc` sources are newer.
  - Retail game dirs (2026-10-10; the old `Downloads\Unreal Tournament`
    path is gone): `D:\old\Unreal Tournament\Unreal Tournament\` (the copy
    the playtest logs come from) and
    `C:\Users\kkragoth\dev\Unreal Tournament\Unreal Tournament\`. Deploy
    `RagdollPkg.dll` (and `.u` after `ucc make`) to BOTH `System\` folders.
    Both installs' `RagdollPkg\` folders are junctions into THIS repo's
    `RagdollPkg\` (recreated 2026-10-10 with `mklink /J`), so `ucc make` run
    from either `System\` compiles the repo sources
    (`...\Ut99PubSrc` is the retired monorepo - do not edit or deploy from it).
  - Non-interactive `ucc make`: pipe the header answer, e.g. in PowerShell
    `"N`nN`n" | .\UCC.exe make` (answer N keeps the hand-maintained header).
  - `ucc make` binds native functions by the DLL export name
    (`intARagdollCarcassexec<Name>`). New native exec functions must be
    built/deployed in the DLL **before** running `ucc make`, or make fails with
    "Can't find ... in RagdollPkg.dll".
  - When `ucc make` prompts "RagdollPkgClasses.h needs to be updated (Y/N)":
    answer **N** and keep the hand-maintained
    `RagdollPkg\Inc\RagdollPkgClasses.h` (uses `-1` native indices, matched by
    name at runtime). If you do answer Y, rebuild the DLL against the
    regenerated header.
- Playtest: `UnrealTournament.exe
  DM-Deck16][?game=Botpack.DeathMatchPlus?MinPlayers=8?mutator=RagdollPkg.RagdollMutator,RagdollPkg.RagdollTestMutator
  -log` from the retail `System\`. Logs land in `System\UnrealTournament.log`.

## Physics tuning knobs (no rebuild)

`RagdollCarcass` has globalconfig floats read natively by name
(`RagdollFloatProp`, same property-walk as `bBoxMannequins`), so they can be
tuned in the console and apply to the next corpse:
`set RagdollPkg.RagdollCarcass JointFrictionScale 0.5` (joint friction,
1 = defaults of 0.15-0.8 Nm), `JointLimitScale` (cone/hinge ranges),
`AngularDamping` (per-body, default 0.15), `DeathSpin` (random rad/s per
body at death, default 2.0), `PoseVariation` (spread of the random spawn
pose built by `ApplyDeathPose`; 0 = every corpse gets the fixed weapon-aim
pose, 1 = default, up to 2). Persisted under `[RagdollPkg.RagdollCarcass]`
in UnrealTournament.ini. The spawn pose is forward kinematics down the bone
chain with a random local rotation per joint (arms forward/out + elbow flex,
hips spread + knee flex, spine/head tilt), so it always satisfies the joint
limits. Walk-over pushes (`execWalkOverPush`) act per body on the closest
point of the body box to the pawn cylinder, as an off-centre impulse plus a
radial shove, so trodden limbs rotate and splay instead of the corpse
sliding as one block. The 2026-10-10 defaults replaced joint brakes of
1.5-2.4 Nm (15-50% of the limbs' gravity torque, which froze limbs 20-35 deg
from vertical) and ~doubled every joint range. Dataset body boxes are capped
in height and checked for non-adjacent overlap in `build_rig_mixamo.py`
(the spine box used to reach into the thigh boxes and jam the hips).

## Self-collision, corpse stacking, per-model joints

Implemented 2026-10-10 (evening), see `docs/handoff-2026-10-10-next.md`
"Status" for the headless measurements. NOT yet playtested in the client.

- Self-collision: Box3D joints default to `collideConnected = false`, so a
  body could pass through the one body it is jointed to (upper arm through
  the spine box = "arms pass through the torso"). The shoulder and hip
  joints now collide (`bSelfCollide`, globalconfig bool on `RagdollCarcass`,
  default true, read at `BuildRagdoll`); their child hulls are trimmed at
  the socket by the rest-pose overlap (`RagdollTrimChildBox`, offset hull
  via `b3MakeOffsetBoxHull`, body origin unchanged so skinning is
  unaffected). Elbows/knees/neck stay non-colliding (their boxes overlap by
  construction). Non-adjacent bodies and other corpses always collided
  (same world, same category); corpses rest on each other (headless pile:
  10-12 of 16 pelvises >15 UU above the floor).
- Diagnostics: `RagdollPkg: contacts <corpse> sync=N geom= self_joint=
  self_other= corpses= asleep= pairs:` for the first 8 corpses at sync
  50/150/300 (`LogRagdollContacts`; `Nj` = jointed pair, `N-frag` =
  fragment body). Every body carries its `ARagdollCarcass*` as Box3D user
  data (`FRagdollState::Owner`). `RagdollTestMutator` also runs a
  two-corpse stacking test (`stack lowerTopZ=... pelvisXYdist=`) and logs
  the gore pile (`pileTopZ`, `raisedPelvises`).
- Per-model joints: `tools/rigs/<Sel>.tuning.json` (degrees / Nm / UU
  anchor offsets, one line per joint) -> `python tools/build_joint_tuning.py`
  -> `RagdollPkg/Src/RagdollJointTuning.h` (`GRagdollJointTuning[]`, looked
  up by dataset name in `BuildRagdollImpl`; global table
  `GRagdollDefaultJoints` for models without a file). Rebuild the DLL only
  (no `ucc make`). The five files currently equal the global defaults
  (`--init` writes them), so behaviour is unchanged until someone edits one.
  Console multipliers still apply on top. Log line:
  `RagdollPkg: joints <corpse> table=<dataset|global> ...`. Hinges also carry
  `rest_deg` (spec slot `L2`): the joint spring's rest angle, elbow +20 /
  knee -15 by default, because a spring aimed at exactly straight parked
  most elbows and knees ON the hyperextension stop (stiff straight limbs).
- Deploy check before reading a playtest log: compare the install's
  `System\RagdollPkg.dll` / `.u` timestamps with the repo's `System\` and
  the other install. The 2026-10-10 "ragdolls dont look ok" screenshot came
  from D:\old running a build two deploys behind (the game had held the
  files locked), so every limit in its `jointangles` lines was the old one.
- Joint geometry facts (2026-10-10 playtest): Box3D CLAMPS every spherical
  cone to 90 deg and centres it on frame A's z axis. With identity frames
  that is the hanging limb, so arms could never pass horizontal and legs
  raised at most the hip cone. Ball joints therefore carry a `cone_axis`
  (bind space, left = -X, front = +Y, up = +Z): shoulders aim out+forward
  (whole front/outer hemisphere), hips ~36 deg forward of down. The bind
  pose sits off-centre inside the cone with zero twist (`RagdollShortestArc`
  frames in `AddJoint`). Every joint also has a weak spring toward the bind
  pose (`JointSpringHertz` 0.8 / `JointSpringDamping` 0.8, globalconfig; 0
  = off) so limbs stop settling at a limit. `RagdollPkg: jointangles`
  (first 8 corpses, sync 50/150/300) prints every joint's angle against its
  limits with `!` at a limit; `RagdollPkg: underfloor` lists bodies with
  level geometry just above them and none below.
- Gore roll (2026-10-10, "gibbing way too easily"): native `Gib` cuts ONE
  part mostly, two rarely, three very rarely (odds scale with damage, 25%/5%
  at 80), forearms/shins below 90 damage and whole limbs above, spine/neck
  never short of a TOTAL gib at 250+ damage. Script `Blast()` wraps it (once
  per tick: a gibbing kill arrives via ApplyDeathGore AND the engine's
  ChunkUp). Corpse hits: explosive >= 40 damage = Blast; smaller hits
  accumulate per part (`PartThreshold`); only 250+ or 3x Mass cumulative
  gibs outright. Body fragmentation (random-plane chunks with fan caps, the
  "torso collapsing into pieces") is behind `bFragmentBodies`, default off.
- Shoulder pads: `build_rig_mixamo.py` routes clavicle weights on verts
  lateral of the shoulder anchor to the upper arm (binds only; boxes and
  anchors unchanged). Regenerate with the documented header command.
- Leg stance: the selection meshes stand in a contrapposto (one hip ~5 UU
  ahead, shins tilted back 5-18 deg) while the physics skeleton is
  mirror-averaged, so every shin's skin sat ~3.5 UU off its box (boots
  into the floor, legs bent wrong). `unpose_legs` in `build_rig_mixamo.py`
  now straightens both legs with weight-blended rigid transforms before
  anything is derived. Check with the offline vertex-cloud-vs-box means
  (should be ~0) after any rig regeneration.
- Overhang hulls: skin past the distal faces of the shins, forearms and
  head (toes, heel, hands, skull) gets a second hull on the same body
  (`RagdollAddOverhangHull`, logged as `RagdollPkg: overhang hull`), so
  feet no longer clip into the floor on a face-down corpse.

## Gore (dismemberment / decapitation / gibbing)

Planar cuts exist but are OFF by default (`bPlanarCuts`, globalconfig bool;
the user found them "a bit broken visually"); the default split is the
dominant-bone one described in `docs/plans/gore.md`.

Implemented 2026-10-10 per `docs/plans/gore.md` (read its "Implementation
notes" first). Natives `Dismember` / `RemovePart` / `Gib` /
`GetBodyLocation` / `NearestBody` / `BodyAttached` live in
`RagdollCarcass.cpp`; the skin split + stump caps in
`RagdollPkg/Src/RagdollGore.cpp`. With `bPlanarCuts` on, every cut is a
PLANE fitted to the heat-weight seam ring (NOT the skeleton anchor: the
shoulders sit above the neck anchor on every model, see
`docs/gore-planar-cut-findings.md`), seam faces are clipped against it and
the planar rim is fan-capped flat with a dark cap texture (material slot 7);
fragments are random planes; fused regions (armpit, collar) fall back to
duplicated skirts with small caps. With it off (default) every seam face
takes the duplicated-skirt path with full centroid fans.
Verify seam changes offline first: `tools/plane_cut.py` (parses
`RagdollDatasets.h`, same algorithm) + `tools/render_groups.py` with
ABSOLUTE paths (`RG_RES=1400` for close-ups). Wound marks (`PaintWound`)
take the shot direction and mark the skin face along that ray (corpse hits
land on the collision cylinder, far from a lying corpse's skin). Script
triggers are in `RagdollCarcass.uc` (`ApplyDeathGore`, `TakeDamage`,
`ChunkUp`) with the killing blow recorded by
`RagdollMutator.MutatorTakeDamage`. Knobs
(globalconfig, console-settable): `GoreLevel` (0 off, 1 decap+gib, 2 limbs;
default 2), `DismemberScale` (threshold multiplier), `bPlanarCuts` (planar
clipped cuts, default false). The package still
depends on Engine only: Botpack classes are `DynamicLoadObject`ed.
`RagdollSpawnNotify` intercepts ONLY `UTHumanCarcass` (chunks/heads keep
stock behaviour). Headless check: `RagdollTestMutator` drives the gore
natives on a `ucc server`; `ucc server` cannot run from a path with spaces,
so `subst U: "<install>"` first and run `U:\System\UCC.exe server ...
-log=ragdolltest.log` (remove with `subst U: /D`).

## Ragdoll real-mesh skinning (ON; runtime-toggleable)

Real-mesh skinning is the default, gated at runtime by dataset availability
(`bRealMesh` when `S->VertBinds.Num() == FrameVerts` in
`RagdollPkg\Src\UnRagdollMesh.cpp`). Box mannequins appear when a model has no
dataset OR when the class config flag `bBoxMannequins` is true — toggle with
`set RagdollPkg.RagdollCarcass bBoxMannequins 1|0` (console or ini), no
recompile. The gate lives in one place per side: `RagdollForceBoxes()` wrapping
`FindRagdollDataset` in both `RagdollCarcass.cpp` (physics skeleton) and
`UnRagdollMesh.cpp` (mesh build). Read `docs/handoff.md`,
`docs/skinning-findings.md`, and `docs/plans/real-mesh.md` before touching this
feature.

An earlier "weld surgery" experiment (dropping hand<->leg fused faces +
fan-capping the rims to fix "missing parts in legs") was investigated and
**reverted**: the caps overlapped kept faces because the seam loops are
mis-traced. Do not resume that surgery as-is; see "Re-enable path" in
`docs/handoff.md`.

## Regenerating datasets (T-pose + Blender heat rig)

`RagdollPkg/Src/RagdollDatasets.h` is committed; regenerate only when rig
logic changes. The ACTIVE source of truth is `docs/tpose-rig-workflow.md`:
`tools/tpose_rig.py` (segment arms by mesh connectivity -> swing them into a
T-pose -> derive a Mixamo-named armature -> Blender 3.6 bone-heat weights via
`tools/blender_autorig.py` -> un-pose) writes `tools/rigs/<Sel>.json`, then
`tools/build_rig_mixamo.py ... --header RagdollPkg/Src/RagdollDatasets.h`
emits the header (its `--mixamo-dir` defaults to `tools/rigs`). Verify with
`tools/render_groups.py` (OBJ import must be `forward_axis=Y, up_axis=Z`).
`tools/mixamo_rigs/` (Mixamo FBX dumps) and `build_rig.py`'s own binders are
legacy fallbacks only: Mixamo bound the hanging hands to the LEG bones and
the range-based binders mislabelled feet/arms. Renders and `.blend` files
land in `tools/rig_verify/` (gitignored).
