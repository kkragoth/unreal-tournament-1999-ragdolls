# RagdollPkg Real-Mesh Skin — Handoff

Status date: 2026-08-08. Read this first, then the relevant section in
`docs-skinning-findings.md` and `AGENTS.md` before doing anything.

## Where things stand (TL;DR)

Real-mesh corpses now work end-to-end:

- The complete source Selection mesh is exported **untouched** (no face dropping,
  no added geometry). It matches what you see in Blender.
- Each model's mesh is yaw-corrected to a front-facing bind space, bound rigidly
  (1 bone per vertex) to a 12-body physics skeleton derived per-model.
- Feet are **skipped from the ragdoll**: foot verts bind rigidly to the shins,
  and the real-mesh skeleton has 12 bodies (no foot bodies). Box fallback keeps
  14.
- Playtest log is clean: no crash, no NaN/inf, no stretched triangles
  (`RealTris spike=0`), physics pelvis velocity stays 0-5 m/s (no more
  explosion where bodies flew 12+ m apart).

## The two big bugs found + fixed this session

1. **Hip joint anchor sign error** (`tools/build_rig.py` `compute_anchors`):
   `hip_z = c[8][2] - (c[8][2] - c[10][2]) / 2.0` put the hip anchor at the
   SAME z as the knee (-22.3), so the thigh pivoted around its bottom end →
   legs split and looked ~4x size / not a single consistent limb. Fixed to `+`
   so hip sits between pelvis(0) and thigh(-13.5): hip z≈-4.8, knee≈-22.3,
   ankle≈-35.9. This was the "legs split / 4x size" symptom.

2. **Over-aggressive face dropping** (removed): the old `_is_arm_leg_face`
   filter deleted ~10% of the whole mesh (faces along the arm-torso seam —
   the arms hang down against the body in the bind pose, so arm-vert ↔
   torso-vert faces ARE the character's skin, not "bridges"). Deleting them
   caused the hole-filled / disconnected mesh seen when importing the OBJ into
   Unity. Mesh is now exported complete.

Also fixed earlier: negative shin half-height (inverted shin box),
smooth-skinning 3-color overlap → rigid single-bone binding, foot boxes derived
from scattered source verts (reverted — not needed since mesh is complete).

## Current tooling

- `tools/build_rig.py` — full pipeline per Selection mesh:
  - extract frame 0 → center + yaw-correct (mirror + leg-split facing)
  - `derive_bones` → 14 bone centers/halves from geometry (region bands)
  - `_enforce_clearance` → push arm boxes outward off the legs
  - translate pelvis to origin, `compute_anchors` (13 joints)
  - `bind_verts` → rigid 1-bone bind with arm/leg exclusion + feet→shin +
    forearms→forearm override (feet skipped from ragdoll)
  - verify: no bone starvation, bones on clusters, no non-adjacent
    interpenetration
  - emit `RagdollPkg/Src/RagdollDatasets.h` (5 models) + OBJ overlays
- Native side (`RagdollPkg/Src/RagdollCarcass.cpp`): dataset-driven skeleton
  (`FindRagdollDataset`), `S.NumBodies = DS ? 12 : 14`, foot joints skipped
  when DS present, calmer death impulse + damage impulse so long real-mesh
  limbs don't flail apart.
- `tools/rig_verify/` — OBJ + PNG overlays (front/3q/back) per model; bone
  colors show binding. Regenerate with `--render` (needs BLENDER_EXE or on
  PATH).

## Commands

```
# regenerate header + overlays + renders
BLENDER_EXE="C:\Program Files\Blender Foundation\Blender 3.6\blender.exe" `
  python tools/build_rig.py "<retail>\System\Botpack.u" --obj-dir tools/rig_verify --render

# rebuild native
MSBuild build\RagdollPkg\RagdollPkg.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:BuildProjectReferences=false

# deploy + playtest (native-only changes need no ucc make)
copy System\RagdollPkg.dll <retail>\System\
<retail>\System\UnrealTournament.exe "DM-Deck16][?game=Botpack.DeathMatchPlus?MinPlayers=8?mutator=RagdollPkg.RagdollMutator,RagdollPkg.RagdollTestMutator -log"
```

## OPEN QUESTIONS — ask the user / decide

1. **Natural pose vs. arm fold.** Right now the bind pose is natural
   (arms hang down, hands against the thighs). The bone-color renders show the
   hand region overlapping the thigh/shin colors because the hands physically
   sit against the legs. This is cosmetic in the overlay; it was the motivator
   for "Strategy A" (fold forearms up 150° about the elbow so hands leave the
   ankles). The fold scatters the mesh visually and the user did not like the
   result, so it is OFF (`--fold 0` default). Decide whether to pursue the
   fold again (e.g. fold mesh + Skeleton together so bones match, or a
   smaller fold angle) or accept the natural pose. NOTE: with a fold the
   elbow hinge rest angle is no longer straight — native elbow limits
   (-0.12..1.10 rad) would need to accommodate it, or keep skeleton straight
   and only bend the mesh offsets (current approach).

2. **"4x / split legs" — verify fixed in game.** The hip anchor fix is
   committed (15d8f63) and deployed, but the user has NOT re-playtested after
   that fix. First step on resume: relaunch the playtest and confirm legs
   behave like one consistent limb (no splitting/stretching).

3. **Naturalness of deformation.** User: "ragdoll deforms not naturally like
   legs did split and suddenly they were like 4x size and not like single
   consistent thing." That symptom traced to the hip anchor; re-verify. If
   limbs still look rubbery, inspect per-bone jitter / hinge limits
   (`AddJoint` specs in RagdollCarcass.cpp) and the death/damage impulse
   magnitudes.

4. **Strategy B (vert-by-vert assignment)** was approved by the user earlier
   but not implemented. The user does not know Blender, so any per-vertex
   review tool must NOT require Blender (e.g. numbered OBJ + CSV/text table of
   vertex→bone, and an overrides file the rig reads back). Only pursue if the
   auto-binding (rigid, classification-based) still shows wrong regions after
   the above.

## Gotchas / traps

- `ucc make` skips recompilation if `System\RagdollPkg.u` exists — delete it
  before `ucc make RagdollPkg` when `.uc` changes. Native-only changes don't
  need it.
- New native exec functions must be built into the DLL BEFORE `ucc make`.
- `RagdollPkg\Inc\RagdollPkgClasses.h` is hand-maintained with `-1` native
  indices (matched by name). Answer N if `ucc make` asks to regenerate it.
- Verify's "verts near center" check runs on the pre-fold (natural) verts;
  the fold only repositions mesh verts afterward, skeleton unchanged.
- Retail game dir: `C:\Users\kkragoth\Downloads\Unreal Tournament\Unreal
  Tournament\` (its `RagdollPkg\` is a junction to this repo).
- The AGENTS.md note "Real-mesh skinning is OFF / box mannequins" is OUTDATED —
  real-mesh skinning is currently ON and working. Update AGENTS.md when
  confirming the hip fix in-game.

## Reference: mesh/bone numbers (SelectionMale2)

- 306 verts, 501 wedges, 595 faces (complete source mesh).
- 14 derived bones; real-mesh uses 12 (feet skipped). Bone map:
  Pelvis 0, Spine 1, Neck 2, Head 3, UpArmL/R 4/5, ForArmL/R 6/7,
  ThighL/R 8/9, ShinL/R 10/11, FootL/R 12/13.
- Joint anchors (male): hip z≈-4.8, knee z≈-22.3, ankle z≈-35.9,
  shoulder z≈23.3, elbow z≈1.4.

## Commits this session

- 845f291 Complete real-mesh ragdoll: keep full mesh, 12-body skeleton,
  calmer physics
- 15d8f63 Fix hip joint anchor (thigh pivoting around knee)
- (earlier) prior skinning work; see git log for full history.
