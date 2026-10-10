# RagdollPkg Real-Mesh Skin — Handoff

> 2026-10-10: binding source is now `docs/tpose-rig-workflow.md`
> (`tools/tpose_rig.py`, Blender bone-heat weights on a T-posed mesh).
> Everything below about `bind_verts`, region thresholds, topo segmentation
> and the Mixamo rigs is history; the game dir paths in here are stale too
> (see AGENTS.md).

Status date: 2026-08-09. Read this first, then the relevant section in
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

## ACTIVE: binding rework — region thresholds → proximity (2026-08-09)

**SUPERSEDED 2026-08-25 by `docs/plans/topology-rig.md`**: audits proved the
meshes are multi-component (whole arms/armor are separate sheets), there are
ZERO true hand<->leg weld faces (the 58 were classifier artifacts), and
proximity alone cannot disambiguate hands resting on thighs — the plan is
connectivity/geodesic segmentation (FPS landmarks + Voronoi labels), not
proximity. Read that plan before touching `bind_verts`.

The current `bind_verts` uses **region classification** (`fore_idx`/`arm_idx`/
`leg_idx` from `derive_bones`) to force verts to bones. A color-coded render
exposed a real bug in that approach, confirmed by the user in Blender:

- `derive_bones` labels the "arm column" as **every vert with `|x| > 11`** down
  to the floor (`arms_wide` at build_rig.py:143, `fore_idx` at :241). But the
  **feet are also wider than 11** (SelectionMale2 foot verts at x=+17.2, +14.9,
  -17.8), so the outer edges of the feet are classified as "arm column" and
  **bound to the forearm bones (6/7)**. That is why the foot-sole faces read as
  "weld" (arm+leg mixed) and why dropping them punched holes in the soles.
- Numbers (SelectionMale2): 58 weld faces total = **14 at foot level**
  (z<-36.6) + 44 at thigh level. The 14 foot-level ones are genuine FOOT
  geometry mis-bound to forearms (e.g. f256: b6 vert at z=-41.43 = the sole;
  f473: b6 vert at x=-17.8 z=-41.2; f594: b7 vert at x=14.3 z=-41.2).
- So the "hands reach the feet" artifact is a **classifier bug**, not real
  geometry: the hands hang at thigh height (user confirmed in Blender), and the
  flat `|x|>11` sweep cannot tell a hand at thigh height from a wide foot.

Decision: **rewrite binding to be proximity-to-bone-box driven** — each vertex
binds to the closest bone BOX (the physics box the ragdoll actually spawns),
with region labels used only as tie-breakers (foot→shin, arm→armbone, since
feet are skipped from the ragdoll). This is the "Strategy B vert-by-vert"
direction the user approved. The user will iterate visually (Blender/Unity
color-coded render) to confirm bones make sense per model.

Status: docs updated first, code not yet changed. See todo list in session.

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

## Weld-surgery investigation (2026-08-09) — REVERTED, do not resume as-is

An uncommitted experiment tried to fix "missing parts in legs" in the exported
OBJ by dropping the hand↔leg "weld" faces (faces whose verts bind to both
ARM={4..7} and LEG={8..13}) and fan-capping the cut rims, then nudging the
forearm column outward. **It was abandoned and reverted to commit 193039d.**
The backups are in
`C:\Users\kkragoth\AppData\Local\Temp\opencode\surgery_backup\`
(`build_rig_surgery.py`, `RagdollDatasets_surgery.h`). Findings:

- The viewer bind pose hangs the hands against the thighs/shin/feet, so the
  mesh genuinely shares triangles between arm- and leg-bound verts; those
  "weld" faces would spike when the ragdoll separates the limbs. Dropping them
  is the right *idea* but the capping is the hard part.
- The cut seam is NOT a simple boundary. Per hand there are 3 seam loops:
  a big mixed loop (Male2 loop0 n=20 `C L…L C A A A`, loop3 n=18) wrapping the
  whole thigh/shin/foot, a separate all-arm rim loop (loop2 n=12, loop4 n=5),
  and a **kept-triangle island loop** (loop1 n=3 = the kept face f167) sitting
  inside the dropped patch. The island loop got fan-capped back into a
  duplicate of f167 (f167/f555 identical triangles).
- The loop tracer picks `nxts[0]` at each vertex and weaves THROUGH kept faces
  (loop0's seam borders 14 kept LEG faces with kept faces lying inside the
  traced polygon), so the traced loops are not simple polygons and their fan
  caps land on the kept surface.
- Quantitative proof (`cap_overlap.py`): **50 cap edges have same-side apexes
  with the kept face sharing that edge** — e.g. cap 537=(201,43,194) overlaps
  kept face 186 across (43,194); cap 551=(211,220,69) overlaps kept face 130
  across (211,220). The `dot(fn, ref)` flip in `add_cap` only fixes opposite-side
  winding; it cannot fix same-side overlaps.
- Fan-capping a curved 3D strip is fundamentally limited: even the "ear" of a
  rim edge can lie on the kept side, so ear-clipping alone won't fix it either.
  The caps are also z-occluded in deep creases (invisible even with backface
  culling off), so per-face render visibility tests misled the investigation.
- Mesh data today is inspection-only: real-mesh skinning is ON but gated by a
  runtime check (`bRealMesh` when `VertBinds.Num()==FrameVerts` in
  `UnRagdollMesh.cpp`), and the exported OBJ in `tools/rig_verify/` is the
  artifact the user opens. The committed export is COMPLETE (no holes), so the
  "missing parts" came from the surgery itself, not from the committed mesh.

### Re-enable path, if ever wanted (do NOT do lightly)

1. Fix the seam loop tracer to follow true simple boundaries: walk the
   boundary of the dropped-patch region honoring non-manifold seams (a vertex
   shared by >2 seam edges), keep islands as inner loops, and never cross a
   kept face. Verify loops are simple polygons (no self-intersection).
2. Fill each hole with ear-clipping / constrained triangulation oriented
   against each rim edge's kept-face normal — NOT a fan from one anchor.
   Enforce "no triangle fuses an ARM vert to a LEG vert" per triangle, and
   keep islands un-capped (they are kept faces, not holes).
3. Verify with `fold_check.py` (target 0 org+cap same-direction duplicate
   edges) and `enclosed_diff.py` (target ~0 new enclosed-background pixels vs
   the pre-surgery mesh across all 6 views), not with per-face render tests.
4. Then rebuild header + DLL, deploy to retail `System\`, and re-open the OBJs
   in Unity.

## Current tooling

- `tools/build_rig.py` — full pipeline per Selection mesh:
  - extract frame 0 → center + yaw-correct (mirror + leg-split facing)
  - `derive_bones` → 14 bone centers/halves from geometry (region bands)
  - `_enforce_clearance` → push arm boxes outward off the legs
  - translate pelvis to origin, `compute_anchors` (13 joints)
  - `bind_verts` → rigid 1-bone bind (ACTIVE REWORK: region-threshold
    classification is being replaced with proximity-to-bone-box binding;
    region labels become tie-breakers only)
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

0. **"Missing parts in legs" (the weld-surgery motivator) is now understood
   and shelved.** The exported mesh is complete and matches Blender; the
   uncommitted weld surgery (drop + cap the hand↔leg fused faces) produced
   overlapping/invisible caps that READ as "missing parts". It was reverted
   (see the investigation section above). The underlying concern remains:
   when real-mesh skinning actually runs the ragdoll, the fused hand↔leg faces
   will stretch into spikes. Decide whether that ever matters (it is cosmetic
   today since skinning is gated by the runtime `bRealMesh` check, not a
   compile gate) before resuming any surgery.

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

4. **Strategy B (vert-by-vert assignment)** — IN PROGRESS as the binding
   rework. The current region-threshold binding mis-labels wide foot verts
   (|x|>11) as "arm column" and binds them to the forearms; proximity-to-bone-box
   binding replaces it. The user will review per model via the color-coded
   OBJ/PNG overlay (bones as colors) and, if needed, a numbered OBJ + CSV/text
   table + overrides file the rig reads back. The user does not use Blender for
   editing, so any per-vertex review tool must NOT require Blender interaction.

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
