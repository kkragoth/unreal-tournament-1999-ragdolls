# Handoff (2026-10-10, end of day) — next: per-model joints, self-collision

## Status (2026-10-10, later session): both items implemented, headless-verified, NOT yet playtested

Changes are in the working tree (uncommitted at the time of writing) and
DEPLOYED to both installs (`RagdollPkg.dll` + `.u`, built 15:46 / 15:36).

**Self-collision (work 2) — done.** `bSelfCollide` (globalconfig bool,
default true) turns on `collideConnected` for the shoulder and hip joints;
`RagdollTrimChildBox` trims the child hull on the axis with the smallest
rest-pose overlap (thigh/pelvis: 2.5 UU in Z on every model; upper
arm/spine: clear on males/Boss, 0.25-0.44 UU in X on the females) using
`b3MakeOffsetBoxHull`, so the body origin (and thus the skinning) is
unchanged. Offline check of all five datasets: `tools/` has no script for
it, the numbers came from a throwaway parse of `RagdollDatasets.h`; no
non-adjacent pair overlaps at rest. Headless census
(`LogRagdollContacts`, first 8 corpses): non-adjacent self pairs appear
(spine-head, forearm-thigh, pelvis-shin, head-upper arm), the new jointed
pairs touch at rest without jamming, no `Critical`, pelvis speeds settle
to ~0.03 m/s by sync 100. Corpse-corpse: dozens of touching pairs in the
test pile; two-corpse test = upper pelvis 5 UU above the lower at a 40 UU
offset in one run, slid off in others (the spawn kick is random); pile of
16 at one spot = 10-12 pelvises more than 15 UU above the floor. The
dedicated server syncs at ~17 Hz, so the sync=300 census needs a run
longer than ~75 s. To compare with the old behaviour in the client:
`set RagdollPkg.RagdollCarcass bSelfCollide 0` (next corpse).

**Per-model joints (work 1) — infrastructure done, values not yet tuned.**
`tools/rigs/<Sel>.tuning.json` -> `tools/build_joint_tuning.py` ->
`RagdollPkg/Src/RagdollJointTuning.h`, used by `BuildRagdollImpl` when the
dataset name matches (`FindRagdollJointTuning`), else
`GRagdollDefaultJoints`. The five files were seeded with the global
defaults (behaviour identical), ready for hand-editing: ranges in degrees,
friction in Nm, optional `anchor_offset_uu` per joint. Per-model box
scale/mass is NOT in the file (would need the dataset header regenerated).
Still open: ask the user which model looks worst and tune that file first;
the Blender hand-rig exporter (section 4 below) is still missing.

Verified this session: DLL build, `ucc make` (answer N), three ~55 s
headless runs, gore natives still fire (SplitIslands / removed part / gib).

### Later the same day: playtest feedback round (user screenshots)

User items and what was done (all built; deployed to the dev install;
D:\old deploy pending because the user's game held the files):

1. "torso collapsing into several pieces with ugly fan outs" even with
   planar cuts off -> that was the GIB FRAGMENTATION (random-plane chunks of
   head/spine/pelvis/thighs). Now `bFragmentBodies`, default off.
2. "gibbing way too easily; standard explosive should tear 1 (mostly), 2
   (rare), 3 (ultra rare); more damage whole limb, less only hand/foot" ->
   new blast roll in native `Gib` + script `Blast()` (see AGENTS.md). The rig
   has no hand/foot bodies: forearm/shin stand in. Client playtest (18
   corpses, 7 blasts): every blast cut 1-2 parts. Two blasts fired at 16
   damage: those came through the engine's ChunkUp (gib kill) with
   `-Health` = 16, i.e. UT gibbed a pawn at low overkill ('shredded' flak
   path?) - harmless now (1 part), but worth knowing.
3. "some parts go below floor" -> `underfloor` detector added (geometry
   within 0.5 m above a body centre, none below). Two client playtests: 0
   hits in the sampled corpses. Box3D continuous collision is on by default.
   Not reproduced yet; the level mesh only holds solid CSG (no movers, no
   semisolids?) - check `CollectBspGeometry` if the user can say WHERE.
4. "do a playtest and log joint values" -> `jointangles` census; two client
   playtests done (logs in %TEMP%\ut_playtest1.log). Findings: Box3D clamps
   cones to 90 deg (shoulders were pinned at 90 = arms never above
   horizontal); hips sat at the 69 deg cone and 29 deg twist constantly;
   knees fully folded (-115) in most corpses; heads at the neck/head twist
   limits. Hence: cone axes (arms out+forward hemisphere, hips forward),
   tightened ranges (knee 100, hip cone 54, spine/neck/head twists 14/20/20),
   joint springs toward the bind pose (JointSpringHertz 0.8), DeathSpin
   1.2, AngularDamping 0.4, spawn knee flex 0..40 deg. NOTE: the user's
   UnrealTournament.ini persists AngularDamping=0.15 / DeathSpin=2.0, which
   override the new defaults until `set` in the console or edited.
5. "shoulderpads are treated like chest" -> confirmed on the Soldier (34
   clavicle-dominant verts lateral of the shoulder anchor bound to the
   spine); `build_rig_mixamo.py` now routes those to the upper arm
   (Soldier L24/R21, Commando L23/R25, Female1 L45/R39). Render
   `tools/rig_verify/pads_front.png` / `pads_back.png`.
6. "these ragdolls look kinda weird" (5 screenshots, with the cone-axis
   build): limbs at limits - splits, crossed legs, folded knees, twisted
   heads. Addressed by item 4's second half (springs + tighter ranges). Not
   yet seen by the user at the time of writing.
7. "part of feet is missing" (boot cut flat by the floor on a face-down
   corpse) -> two causes found. (a) No collision for skin past the limb
   boxes: `RagdollAddOverhangHull` adds toe/heel/hand/skull hulls. (b) THE
   BIG ONE: every dataset's bind pose is a walking stance (right hip ~5 UU
   ahead of the left, shins tilted back 5-18 deg) and the mirror-averaged
   boxes sat between the legs, so each shin's skin was ~3.5 UU (heel up to
   13) outside its box on all five models. `unpose_legs` in
   `build_rig_mixamo.py` fixes the rig; header regenerated (binds, verts,
   leg boxes changed). This very likely also explains the "parts below the
   floor" report and some of the "weird" leg looks.
8. "legs going like that to arms is weird" (leg raised beside the torso):
   hip cone now 50 deg aimed 27 deg forward / 12 deg out (raise ~77).

Deployed to the dev install only; D:\old is pending (the user's game kept
the DLL/.u/log locked all evening). After the user quits: copy
`System\RagdollPkg.dll` and the dev install's `RagdollPkg.u` to D:\old,
and set AngularDamping=0.4 / DeathSpin=1.2 in its ini (persisted old
values override the new defaults).

You are continuing work on `C:\Users\kkragoth\dev\unreal-tournament-1999-ragdolls`
(UT99 Box3D ragdoll mod, native `RagdollPkg`). Read `AGENTS.md` first (build /
deploy loop, gore section, tuning knobs). Earlier handoffs:
`docs/handoff-2026-10-10-gore.md` (gore system + playtest feedback, now
mostly resolved) and `docs/handoff-2026-10-10.md` (rig pipeline).

## State right now

Everything is committed on `main` and DEPLOYED to both installs (`.dll` and
`.u`). Commits since the gore handoff:

| commit | what |
|---|---|
| `4f41de8` | planar dismemberment cuts (plane fitted to the heat-weight seam ring, seam faces clipped, flat dark caps, fragments by random planes); `PaintWound` takes the shot direction; `tools/plane_cut.py` offline prototype |
| `a761afe` | GPF fix: dangling reference into `VertBinds` while growing it (crashed the client on the test mutator's first sweep) |
| `dfff2db` | planar cuts gated behind `bPlanarCuts` (globalconfig bool on `RagdollCarcass`, DEFAULT OFF) |

**Why the planar cuts are off by default.** The user playtested `a761afe`
(planar always on) and said it is "a bit broken visually" and asked to hide
it behind a flag. Exactly what looked broken was NOT described; candidates to
check in the next playtest with `set RagdollPkg.RagdollCarcass bPlanarCuts 1`:
the armpit fallback patch on torn-off arms, caps on fragment seams (planes
through the torso make large flat cross-sections), the intersection vertices'
skinning (weights copied from the inside endpoint), cap colour. With the
flag off the split is bit-for-bit the morning behaviour (dominant-bone
islands, duplicated skirts, centroid fans; headless forearm 22 seam / 22
caps, neck 42 / 42). `docs/gore-planar-cut-findings.md` has the measurements
behind the planar version, including the important one: the shoulders sit
ABOVE the spine-neck joint anchor on every model, so cut planes must come
from the mesh seam, never from the skeleton anchors.

Wound marks now follow the shot direction (hit momentum) onto the skin, so
shooting a corpse leaves marks (user request); verify in a playtest via the
`RagdollPkg: wound ... ray=1` log lines (cap raised to 60 lines).

Headless check for all of this: `subst U: "<install>"`, run
`U:\System\UCC.exe server DM-Deck16][?game=Botpack.DeathMatchPlus?mutator=RagdollPkg.RagdollMutator,RagdollPkg.RagdollTestMutator -log=ragdolltest.log`
for ~45 s, grep `SplitIslands|cut plane|wound|Critical`, `subst U: /D`.
It does NOT catch everything (the GPF above only showed in the client).

## NEXT WORK 1: per-model joint configuration

User (2026-10-10): "each model should have their own joint configuration",
following the earlier "I don't feel single rigging for female/male different
models is doing best job".

What exists: ONE joint table for every model, hard-coded in
`BuildRagdollImpl` (`RagdollPkg/Src/RagdollCarcass.cpp`, the `AddJoint`
calls: `{Kind, L0, L1, L2, Friction}` = cone half-angle / twist range or
hinge range, friction Nm), scaled globally by `JointLimitScale` /
`JointFrictionScale`. Per model the dataset (`RagdollDatasets.h`,
`FRagdollDatasetEntry`) already carries the 14 box defs (`Bones`: half
extents, centres, mass) and the 13 joint anchors (`Anchors`), derived by
`tools/build_rig_mixamo.py` from `tools/rigs/<Sel>.json`.

Suggested shape:

1. Add an optional per-model joint table to `FRagdollDatasetEntry`
   (`const FRagdollJointSpec* Joints; // 13 or NULL`) plus per-joint anchor
   offsets if wanted. Emit it from `build_rig_mixamo.py` out of a hand-edited
   override file `tools/rigs/<Sel>.tuning.json` (joint ranges in degrees,
   friction, box scale per bone, mass); missing entries fall back to the
   global table so the five models keep working while tuning one at a time.
   `FRagdollJointSpec` is defined in `RagdollCarcass.cpp`; move it to
   `RagdollInternal.h` for the dataset header.
2. Keep the console knobs as multipliers on top (they are read per corpse via
   `RagdollFloatProp`). Consider per-model console overrides too:
   `set RagdollPkg.RagdollCarcass JointLimitScaleFemale 1.2` is cheap to add
   with the same property walk (`RagdollFloatProp` by name).
3. Ask the user which model is worst and start with that one. Render
   `tools/render_groups.py` bone colours (`tools/rig_verify/`) side by side
   male vs female to show the bind differences; `tools/plane_cut.py` can
   also dump per-bone vertex counts.
4. Longer term (still wanted): hand-authored rigs per model in Blender
   (`tools/tpose_rig.py --save-blend` writes the `.blend`; an exporter back to
   `tools/rigs/<Sel>.json` is missing). See the "Feedback 2" section of
   `docs/handoff-2026-10-10-gore.md`.

## NEXT WORK 2: self-collision and corpse-corpse collision

User (2026-10-10): "currently I see arms just pass through torso. The ragdoll
should interact with its own body parts, and maybe other ragdolls, e.g.
possible to stack them".

What the code does today (checked 2026-10-10 evening):

- All corpses live in ONE Box3D world per level (`FRagdollLevelWorld`,
  `UnRagdollGeometry.cpp`). Every body shape has
  `filter.categoryBits = RAGDOLL_BODY_CATEGORY` and the DEFAULT `maskBits`
  (all bits), so by the filter alone bodies of the same corpse and of other
  corpses should collide with each other and with the level mesh.
- Joints are created with the default `collideConnected = false`
  (`b3JointDef.base`, `box3d/types.h:637`), so each body is allowed to pass
  through the ONE body it is jointed to: upper arm through the spine box,
  thigh through the pelvis box, forearm through the upper arm, etc. That is
  the most likely reason "arms pass through the torso": the upper arm is
  jointed to the spine, and the spine box (8 x 6 x 10.5 UU half extents on
  the Commando) is most of the torso. The forearm is not jointed to the spine
  and should already collide with it; verify.
- Bodies are boxes (`b3MakeBoxHull`) sized from the dataset
  (`FRagdollBoneDef`), fragments 80% of their chunk bounds. The 2026-10-10
  dataset fix capped box heights and removed non-adjacent overlaps, so
  non-adjacent boxes should not start interpenetrating. Jointed neighbours DO
  overlap at the joint by design (anchor inside both boxes).

Plan:

1. Measure first, headlessly: in `StepRagdoll`/`SyncRagdoll` (or a test
   native) call `b3Body_GetContactData` per body for a few seconds after
   spawn and log how many body-body contacts exist, split into same-corpse
   vs other-corpse. If same-corpse non-adjacent contacts are zero, something
   beyond `collideConnected` is off (sensor flag, filter, or the solver
   treating the whole ragdoll as one island - check `b3ShapeDef.isSensor`
   and `enableContactEvents`).
2. `collideConnected = true` on the shoulder and hip joints only (upper arm
   vs spine, thigh vs pelvis), with the box ends trimmed so the two hulls do
   not overlap at rest (shrink the child box along its axis by the overlap
   depth, or switch limbs to capsules: `b3MakeCapsule` if Box3D exposes it,
   which also stops boxes catching on edges). Keep elbows/knees/neck as
   non-colliding pairs (their boxes overlap by construction) or trim them
   too and enable one by one; log joint stability (no explosions) with
   `ClearCorpseSpawn` still in place.
3. Corpse-corpse: should already work physically (same world, same
   category). Test by spawning two test corpses on top of each other in
   `RagdollTestMutator` and reading the pelvis Z of the upper one. If they
   sink through, look at `b3WorldDef` (contact hertz / speculative margin)
   and at sleeping: a sleeping corpse must wake when another lands on it
   (`b3Body_SetAwake` / `enableSleep`). The script-side hit cylinder only
   tracks bodies attached to the pelvis; it does not affect Box3D.
4. Cost: self-collision roughly doubles the broad-phase pairs per corpse
   (12 bodies). `SimFreq` 50 / `SimSubSteps` 4 should cope; watch the
   `sync ... count=` cadence in the log with 8+ corpses.

## Other notes

- `ucc make` prompts to regenerate `RagdollPkgClasses.h`: answer N (piped
  `"N`nN`n"`), keep the hand-maintained header. The PaintWound native
  signature changed today; it binds by export name, so no header change was
  needed.
- The PowerShell tool's pre-screen blocks commands that combine
  `Remove-Item` with long quoted exe paths or `U:` paths; split build /
  deploy / subst into separate calls.
- `tools/render_groups.py` needs ABSOLUTE paths for the OBJ and PNG
  (Blender's cwd differs); `RG_RES=1400` env for close-ups; object names
  `Bone_<i>_...` colour by bone, `cap*` dark red.
- Package still depends on Engine only (Botpack classes are
  `DynamicLoadObject`ed). Keep it that way.

### 17:00 playtest: "ragdolls dont look ok" (one screenshot)

The screenshot (arm folded back over the head, shin kinked at the knee,
pelvis twisted, head tucked) came from D:\old, which was still running the
16:21 DLL / 16:01 .u (two deploys behind: the game had held the files
locked all evening) with AngularDamping 0.15 / DeathSpin 2.0 persisted in
its ini. Its `jointangles` lines show the OLD limits (knee 115, hip cone
75, shoulder cone 90, neck twist 29) with knees folded to -115 and spine /
neck / head twist pinned in nearly every corpse. Deployed the 16:52 build +
.u to D:\old and set AngularDamping=0.4 / DeathSpin=1.2 in that ini (the
dev ini has no persisted section).

Headless census of that build (new limits): neck/head twist still pinned
in ~90% of corpses, spine cone at its 26 deg stop in half, ~60% of elbows
on the -3 deg straight stop, knees either on the +3 stop or folded past
-100 (the 17 Hz server overshoots limits by 10-30 deg; the client stays
within ~3). Change made: hinge springs now target a REST angle (spec L2,
`rest_deg` in the tuning files: elbow +20, knee -15) instead of exactly
straight, so limbs settle slightly bent instead of on the hyperextension
stop. DLL rebuilt 17:08, deployed to BOTH installs. Not yet playtested.
