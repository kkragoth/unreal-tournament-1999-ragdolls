# Ragdoll real-mesh skinning findings

Status: **REVERTED / DISABLED**. All corpses render as the procedural box mannequin.
Re-read this file before picking this feature back up. The active plan for picking
it back up is `plans/real-mesh.md` (multi-model Selection-mesh pipeline).

## NOTE (2026-08-09) — binding classifier rework in flight

`handoff.md` "ACTIVE: binding rework" supersedes the region-band wording in this
file and in `plans/real-mesh.md` step 5 ("nearest-bone assignment" is the goal;
the *region-threshold* classifier currently used to force it mis-labels wide foot
verts — |x|>11 — as "arm column" and binds them to the forearms). Proximity-to-
bone-box binding replaces it.

## Decision

The old runtime real-mesh capture path was reverted and stays gated off behind
`RAGDOLL_USE_REAL_MESH` in `RagdollPkg\Src\UnRagdollMesh.cpp` (set to `0`).
Box mannequins remain the shipping look until the `plans/real-mesh.md` pipeline
is implemented.

## Corrected diagnosis (2026-08 validation)

The earlier "T-pose" diagnosis was WRONG. The physics skeleton is **NOT** a
T-pose — `GRagdollBoneDefs` are arms-DOWN (upper arm box z 17..31, forearm
z 0..12, hanging at x=±13, hip-length). The real constraint is only that
**non-adjacent bodies must not interpenetrate at spawn**; adjacent chain bodies
always touch (Pelvis-Thigh, Spine-Neck, ...) and are fine. Arm boxes at x=±10
with a torso half-width of 6 clear everything (verified).

**Why the standing/arms-down mesh capture still failed:** bone placement, not
pose. Skeleton arm bones hang straight down at x∈[4..8], y∈[±13]; the real mesh
hands hang at x≈±16, y≈±12, z≈-36 (ankle length, angled out, ~36 UU below the
skeleton's hip-length arms). Nearest-bone binding on ANY arms-down frame starves
the arm bones (measured on SelectionMale1 f0 after yaw+270 + FitScale:
`UpAL=1, FoAL=3`). The earlier "aligned" overlay only verified torso/feet — the
arms are misplaced in it.

## Old failed approaches (recorded; do not repeat)

- Frame-picker in `UnRagdollMesh.cpp` `CreatePlayerRagdollMesh` rewriting (scan
  all ~700 frames, fit to body length, score nearest-bone evenness).
- Dead-only frame picking, all-frame range scans, any single pose choice.
  None distributed verts well across the 14 bones because the skeleton's arm
  bones were never placed on the mesh's arm line.

## What was tried

- Frame-picker in `UnRagdollMesh.cpp` `CreatePlayerRagdollMesh` rewritten to
  scan **all** animation frames (~700), fit each pose to the skeleton's body
  length, score nearest-bone assignment, plus top-6 candidate diagnostics
  (`cand%i seq=%s f=%i min=%i range=%.0f mean=%.1f`).
- Dead-only frame picking: `pick deadframe=615 fit=0.383 … minBone=0` ->
  arms still 0 verts. Bone counts `{30,96,85,52,0,0,0,0,9,13,11,11,2,2}`.
- All-frame scan by range: `pick bestframe=219 fit=0.755 … range=48`,
  `pick bestframe=198 fit=0.666 … range=70` -> still lopsided, arm bones
  starved, contorted poses.
- Best arm spread ever seen (`Dead2 f=518`): `bones={17,81,39,140,25,15,21,12,1,0,0,0,0,0}`
  -> legs still 0.
- Conclusion: no animation frame (dead or otherwise) distributes verts/faces
  well across 14 bones. Skinning cannot be salvaged. Scanning all frames is
  also expensive (CPU burn) - that cost was the reason diagnostics had to go.

## Real-mesh reference facts

- Mesh has **no `Dead` sequence** -> the old pre-picker code fell back to
  frame 0 of `All` (standing pose). Actual dead sequences that exist:
  `Dead1[525+13]`, `Dead2[538+16]`, `Dead3[554+13]`, `Dead4[567+16]`,
  `Dead7[583+21]`, `Dead8[604+18]`, `Dead9[622+20]`, `Dead9B[642+10]`,
  `Dead11[652+18]` (format: `name[startFrame+numFrames]`).
- `FMeshAnimSeq` (`Engine\Inc\UnAnim.h` lines 34-46): `Name`, `Group`,
  `StartFrame`, `NumFrames`, `Rate`, `Notifys`. Lookup via
  `UMesh::GetAnimSeq(FName)` in `Engine\Inc\UnMesh.h:248-262`.
- Mesh import transform in `CreatePlayerRagdollMesh`:
  `(vert - Origin)` rotated by `RotOrigin`, then scaled by mesh `Scale` and
  actor `DrawScale` -> actor-local space (pelvis at origin, Z up, arms along
  +-X), matching `GRagdollBoneDefs`.
- **Selection viewer meshes** (Botpack.u, `Core.LodMesh`) are the arms-down
  corpse source. Face counts match the in-game models per pair but wedges differ
  (UV splits only — NOT identical topology as previously recorded):
  Commando(332v/642f/487w)->SelectionMale1(332/642/478), Soldier->SelectionMale2,
  SGirl->SelectionFemale2, FCommando->SelectionFemale1, Boss->SelectionBoss.
  All 54 frames, seqs `Breath1..3`, arms down, no weapon. Origins/scales differ
  per mesh. The frame-0 viewer pose is a 3/4 turn: SelectionMale1 needs a +80°
  yaw correction to restore bilateral symmetry (mean mirror dist 2.76 UU).
  See `plans/real-mesh.md`.

## Physics skeleton reference (for re-skinnable rigs later)

- `RAGDOLL_U2M = 0.02` (Unreal units -> meters).
- `GRagdollBoneDefs` (`RagdollCarcass.cpp` lines 44-60): Pelvis(0,0,0),
  Spine(0,0,16), Neck(0,0,25), Head(0,0,33), UpperArmL/R(+-13,6,24),
  ForearmL/R(+-13,8,12), ThighL/R(+-7,0,-12), ShinL/R(+-7,0,-30),
  FootL/R(+-7,0,-37). `RAGDOLL_BONE_COUNT=14`.
- Joint system (`RagdollCarcass.cpp`): `enum EJointKind { JK_Ball, JK_Hinge }`,
  `FRagdollJointSpec { Kind; L0,L1,L2; Friction; }`,
  `AddJoint(S, BodyA, BodyB, AnchorCanonical, DrawScale, Spec)`.
  Config: pelvis-spine ball(0.35,-0.26,0.26,0.6), spine-neck ball(0.61),
  neck-head ball(0.44), spine-upperarmL/R ball(+-13,6,24, cone 1.40),
  upperarm-forearm hinges at (+-13,8,12) range -0.12..1.90, friction 0.5.

## GLog crash constraint (critical, applies to ANY future logging)

`debugf`/`Logf` lines >= ~1000 chars overflow `Core\Inc\FOutputDeviceFile.h`'s
`TCHAR Ch[1024]` (lines 69-71) -> AV `0xc0000005` / fail-fast `0xc0000409`.
Every future log line must stay well under 1000 chars.

## Re-enabling the feature (the active plan)

Follow `plans/real-mesh.md`. Summary: derive a per-model skeleton rest pose from
each Selection mesh's geometry (absorbs model size variance), bind in that pose,
precompute offline; runtime spawns bodies at the derived centers and emits
`boneWorld * bindOffset` via the existing `GetFrame`. The box mannequin stays
the fallback.

1. Build `tools/build_rig.py`; Blender-verify the Commando/SelectionMale1
   dataset (arms included, not just torso/feet).
2. Native: dataset loader + `BuildRagdoll` bodies from derived centers +
   Selection-mesh lookup + MultiSkins mapping; keep `RAGDOLL_USE_REAL_MESH`-style
   gating off until the dataset path replaces the capture path.
3. Native-only edits: MSBuild `build\RagdollPkg\RagdollPkg.vcxproj`
   `/p:Configuration=Release /p:Platform=Win32 /p:BuildProjectReferences=false`,
   copy `System\RagdollPkg.dll` to the retail `System\`.

## Relevant files

- `plans/real-mesh.md` — the active multi-model real-mesh plan.
- `RagdollPkg\Src\UnRagdollMesh.cpp` — `CreatePlayerRagdollMesh`
  (gated, contains the frame-scan picker + `FitScale` + diagnostics).
- `RagdollPkg\Src\RagdollCarcass.cpp` — `GRagdollBoneDefs`, joint config,
  the `S.Mesh = CreatePlayerRagdollMesh(...)` call site (~line 281).
- `RagdollPkg\Inc\RagdollInternal.h` — `RAGDOLL_BONE_COUNT=14`, `BONE_*`,
  `FRagdollState`.
- `tools/extract_mesh.py` — package parser (proven; Scale-then-rotate, engine
  yaw sign). `tools/ragdoll_frame_analysis.py` — BONES table + OBJ export.
- `selection_male1_f0.obj` / `ragdoll_skeleton_align.obj` — note: the align OBJ
  only matches torso/feet; arms are misplaced (see corrected diagnosis).
- `Core\Inc\FOutputDeviceFile.h` — GLog 1024-char limit.
- `Engine\Inc\UnMesh.h`, `Engine\Inc\UnAnim.h` — anim sequence APIs.
