# Ragdoll real-mesh skinning findings

Status: **REVERTED / DISABLED**. All corpses render as the procedural box mannequin.
Re-read this file before picking this feature back up.

## Decision

The user chose to revert real-mesh skinning entirely. Real-mesh capture is
**fundamentally** incompatible with the physics skeleton, not a tuning bug.
The feature is gated off behind `RAGDOLL_USE_REAL_MESH` in
`RagdollPkg\Src\UnRagdollMesh.cpp` (set to `0`). Box mannequins are the
shipping look.

## Why it cannot work (the fundamental mismatch)

- The Box3D physics skeleton must spawn in a **T-pose** (limbs spread).
  Bodies that spawn interpenetrating (a standing corpse's arms inside its
  torso) explode at spawn time.
- A freshly killed player's real mesh is in a **standing pose** (arms at
  sides). Skinning that pose onto the spread T-pose skeleton forces the
  nearest-bone vertex assignment to glue arms to the torso (arm bones starve
  to 0 verts, torso bones swallow everything) and the verts tear when the
  bodies swing.
- Therefore a standing real-mesh capture can never skin correctly. There is
  no combination of frame picking, fitting, or scaling that fixes it.

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

## Re-enabling the feature (if ever revisited)

1. In `UnRagdollMesh.cpp`, set `RAGDOLL_USE_REAL_MESH` to `1`.
2. Realize the standing-corpse/T-pose-skeleton problem above is unsolved.
   A viable path would need a body-space skinned mesh that does NOT depend on
   the physics pose (e.g. deform the mesh from the bones at a fixed rest pose,
   or use a new spawn pose that is spread without interpenetration).
3. Native-only edits: MSBuild `build\RagdollPkg\RagdollPkg.vcxproj`
   `/p:Configuration=Release /p:Platform=Win32 /p:BuildProjectReferences=false`,
   copy `System\RagdollPkg.dll` to the retail `System\`.

## Relevant files

- `RagdollPkg\Src\UnRagdollMesh.cpp` — `CreatePlayerRagdollMesh`
  (gated, contains the frame-scan picker + `FitScale` + diagnostics).
- `RagdollPkg\Src\RagdollCarcass.cpp` — `GRagdollBoneDefs`, joint config,
  the `S.Mesh = CreatePlayerRagdollMesh(...)` call site (~line 281).
- `RagdollPkg\Inc\RagdollInternal.h` — `RAGDOLL_BONE_COUNT=14`, `BONE_*`,
  `FRagdollState`.
- `Core\Inc\FOutputDeviceFile.h` — GLog 1024-char limit.
- `Engine\Inc\UnMesh.h`, `Engine\Inc\UnAnim.h` — anim sequence APIs.
