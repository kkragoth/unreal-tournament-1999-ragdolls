# Plan: multi-model real-mesh ragdolls via Selection meshes (ARCHITECTURE)

Status: **PLANNING**. Rewrites the earlier `real-mesh.md` external-extraction idea with
the per-model pipeline and corrects the "T-pose" diagnosis in `docs-skinning-findings.md`.
Read that findings doc first — it records every prior experiment.

## Corrected diagnosis (this session's validation)

- The physics skeleton is **NOT a T-pose**. `GRagdollBoneDefs` are arms-DOWN:
  upper arm box z 17..31, forearm z 0..12, hanging at x=±13 (hip-length arms).
  The docs' "must spawn in T-pose (limbs spread)" is misleading.
- The real physics constraint is only: **non-adjacent bodies must not interpenetrate
  at spawn** (Box3D explodes interpenetrating bodies). Adjacent chain bodies always
  touch (Pelvis-Thigh, Spine-Neck, ...) and are fine.
- The reason real-mesh skinning failed is **bone placement, not pose**:
  - The skeleton's arms only reach the hips (forearm bottom z≈12);
  - The Selection mesh's hands hang at z≈-36 — the arms reach the ANKLES, ~36-48 UU
    below the skeleton's hip-length arms (measured on SelectionMale1 frame 0:
    hands reach z=-39.6).
  - Nearest-bone binding on ANY arms-down pose starves the arm bones (measured:
    `UpAL=1, FoAL=3` on SelectionMale1 f0 even after yaw+270 + FitScale).
- The earlier "aligned" overlay only aligns torso/feet; the ARMS are misplaced in it
  (0 mesh verts land inside the skeleton arm boxes). Do not trust it for the arms.
- **Selection viewer pose is a 3/4 turn, not front-facing.** SelectionMale1 frame 0
  bbox is X[-18.5,7.0]; a +80° yaw correction restores bilateral symmetry (mean
  mirror distance 2.76 UU). Any front/back symmetric bone derivation must rotate
  the frame to bind space FIRST (see "Yaw correction" below).

## Model inventory (Botpack.u, all `Core.LodMesh`)

    name            verts faces wedges mats frames world X   Y   Z   origin
    Commando        332   642   487    4    700    56.0 39.6 83.5  (-150,40,0)
    Soldier         309   595   504    4    700    52.1 40.4 75.9  (-150,40,0)
    Boss            313   582   446    5    700    57.4 40.4 78.5  (-150,40,0)
    SGirl           311   599   507    4    681
    FCommando       351   674   537    3    681
    SelectionMale1  332   642   478    4     54    25.5 39.8 84.3  (-135,60,0)
    SelectionMale2  309   595   501    4     54    26.6 39.9 79.9  (-135,60,0)
    SelectionFemale1 351  674   540    3     54    26.1 56.1 82.7  (0,-100,0)
    SelectionFemale2 311  599   513    4     54    25.3 57.2 81.4  (0,-100,0)
    SelectionBoss   313   582   442    5     54    27.4 39.4 83.4  (-135,60,0)

- **Per-model mapping (face counts match per pair; wedges differ = UV splits only):**
  Commando↔SelectionMale1, Soldier↔SelectionMale2, Boss↔SelectionBoss,
  SGirl↔SelectionFemale2, FCommando↔SelectionFemale1. Unknown model → box fallback.
- **CORRECTION vs earlier plan:** Commando/SelectionMale1 do NOT share topology —
  faces match in COUNT (642=642) but wedges differ (487 vs 478). The dataset path is
  unaffected: we build the `URagdollMesh` ENTIRELY from the Selection dataset
  (verts/faces/wedges/materials), never from the live pawn mesh geometry. The live
  mesh only contributes its NAME (dataset lookup) and the pawn's `MultiSkins`
  (texture slots; renderer reads `Owner->MultiSkins` via `UMesh::GetTexture`).
- Variance is real: heights 75.9..84.3, female Y-spans 56 vs male 40. **A single
  canonical skeleton cannot fit all models** -> the skeleton rest pose must be
  derived per model (or parameterized by per-model joint positions).

## Model inventory (Botpack.u, all `Core.LodMesh`)

    name            verts faces frames  world X   Y   Z   origin
    Commando        332   642   700    56.0 39.6 83.5  (-150,40,0)
    Soldier         309   595   700    52.1 40.4 75.9  (-150,40,0)
    Boss            313   582   700    57.4 40.4 78.5  (-150,40,0)
    SelectionMale1  332   642    54    25.5 39.8 84.3  (-135,60,0)
    SelectionMale2  309   595    54    26.6 39.9 79.9  (-135,60,0)
    SelectionFemale1 351  674    54    26.1 56.1 82.7  (0,-100,0)
    SelectionFemale2 311  599    54    25.3 57.2 81.4  (0,-100,0)
    SelectionBoss   313   582    54    27.4 39.4 83.4  (-135,60,0)

Variance is real: heights 75.9..84.3, female Y-spans 56 vs male 40. **A single
canonical skeleton cannot fit all models** -> the skeleton rest pose must be
derived per model (or parameterized by per-model joint positions).

## Architecture

Offline per-model pipeline (tools/), one-time per player model; runtime loads a
precomputed dataset. No runtime frame scanning, no CPU burn.

### Offline: `tools/build_rig.py` (per model)

1. Extract the Selection mesh with `extract_mesh.py` (verts, faces, wedges,
   materials, Origin/Scale/RotOrigin, frame 0 = arms-down viewer pose).
2. **Yaw-correct to bind space.** Search the corrective yaw that minimizes
   bilateral symmetry error (mirror-across-X distance; SelectionMale1 = +80°,
   mean mirror dist 2.76 UU). Rotate all frame-0 verts by it so the body faces
   forward (sagittal plane = X=0). This is what makes the arms a clean `|x|`
   region separate from the torso (review #2 resolved: no more 3/4-turn tear).
3. **Derive the 14 bone centers from the mesh geometry** in bind space (vertex
   clusters): head/neck/torso/pelvis from Z bands, arms by `|x|` clear of the
   torso half-width, legs below the pelvis; upper-arm/forearm split at the
   derived elbow line so the forearm reaches the mesh's actual hand line
   (z≈-36, review #5 resolved by construction). Mirror-average the L/R pairs so
   the physics skeleton is bilaterally symmetric. Absorbs every model's
   size/proportion variance. (Do NOT use canonical `GRagdollBoneDefs` centers —
   they are arm-misplaced.)
4. Size each bone box to the local geometry; compute the 14 joint anchors from
   the derived centers (midpoint/limb-line anchors matching the canonical joint
   set). Verify **non-interpenetration** of non-adjacent bodies (arms clear
   torso/legs) in the derived pose.
5. Bind: nearest-bone assignment of every vertex in bind space ->
   per-vertex `{boneIndex, boneLocalOffset}`. Assert every bone gets verts (no
   starvation) AND that each bone's own verts are within its box (tight fit).
   **2026-08-09: this is the proximity-to-bone-box binding (see handoff.md
   "ACTIVE: binding rework") — NOT region-threshold classification, which
   mis-labels wide feet as "arm column".**
6. **Verify externally (headless Blender, `blender.exe -b`):** render an OBJ
   overlay of the mesh + derived skeleton boxes from front and 3/4 views; check
   BOTH torso AND arms/legs (review #1 resolved: arms verified per model, not
   assumed). Save the renders as PNGs in `tools/rig_verify/`.
7. Emit a dataset (generated C header `RagdollPkg\Src\RagdollDatasets.h`):
   bind-space verts/faces/wedges/materials + derived bone centers/half-extents/
   masses + joint anchors + per-vertex bind data. Wedges already include the
   SpecialVerts offset so they index the dense vert array directly.

### Runtime (RagdollPkg native)

1. On `BuildRagdoll`, read `Carcass->Mesh->GetName()` (the dead pawn's `ULodMesh`,
   e.g. Commando) -> look up its Selection-mesh dataset in the per-model table
   (live-name → dataset mapping above). Unknown model -> fall back to the box
   mannequin (unchanged).
2. Build `URagdollMesh` ENTIRELY from the dataset (verts/faces/wedges/materials +
   `S.VertBones`/`S.VertOffsets`), using the existing scaffolding in
   `CreatePlayerRagdollMesh` (`UnRagdollMesh.cpp`): LOD pins, `FaceLevel`,
   `CollapseWedgeThus`/`CollapsePointThus`, `BuildMeshAdjacency` (review #7).
   `Mesh->Scale/Origin/RotOrigin` stay neutral (GetFrame emits world space).
3. Spawn the 14 Box3D bodies at the **derived** rest centers (not canonical
   defs), scaled by `DrawScale`; add the 14 joints with the **derived** anchors
   and the SAME joint specs as today (ball/hinge, cone/limits, friction) so the
   tuned feel is preserved (review #3: specs unchanged, only anchor points move
   with the derived skeleton; box remains the default gate/fallback).
4. `GetFrame` emission is unchanged: `boneWorld * bindOffset` per vertex (the
   existing real-mesh branch already does this).
5. Textures come from the Carcass actor: `Initfor` already copies
   `Pawn(Other).MultiSkins[0..3]` onto the corpse, and the renderer resolves
   per-face textures through `UMesh::GetTexture(Count, Owner)` =
   `Owner->MultiSkins[Count]`. Populate `Mesh->Textures[0..3]` from
   `Carcass->MultiSkins` (fallback `Skin` for slot 0) so the corpse wears the
   player's skin (review #6).

### Data lifetime / memory

Each dataset is ~332 verts + 642 faces + wedges + materials + 14 bone defs +
332 bind entries — ~15 KB. Embedded as static arrays in the generated
`RagdollPkg\Src\RagdollDatasets.h` and compiled into the DLL (no runtime IO).

## Deliverables

- `tools/build_rig.py` (yaw-correct + derive skeleton + bind + verify + emit
  datasets + headless-Blender render)
- Generated `RagdollPkg\Src\RagdollDatasets.h` (SelectionMale1/2,
  SelectionFemale1/2, SelectionBoss) + verify PNGs in `tools/rig_verify/`
- Native: dataset loader (`CreateDatasetRagdollMesh` in `UnRagdollMesh.cpp`) +
  live-mesh-name → dataset lookup + `BuildRagdoll` body placement from derived
  centers + derived joint anchors
- Updated `docs-skinning-findings.md` with the corrected diagnosis + topology
  correction

## Out of scope (first pass)

- Smooth multi-bone weights (rigid 1-bone skinning first).
- Custom/modded player meshes (fall back to box until a dataset exists).
- Runtime re-rigging (everything is precomputed offline).

## GLog constraint (critical)

Any runtime logging must stay well under 1000 chars per line — `TCHAR Ch[1024]`
in `Core\Inc\FOutputDeviceFile.h:69-71` overflows to AV `0xc0000005`.

## Adverse review — resolutions (each risk now has a concrete answer)

1. **Arms were never actually aligned** → RESOLVED BY CONSTRUCTION. Bone centers
   are derived from the mesh's own geometry (arm region by `|x|` clear of the
   torso, split at the derived elbow), so the bones sit ON the mesh's arm line by
   definition. Headless-Blender render of mesh+skeleton OBJ overlay is a hard
   gate before any native work.
2. **Selection viewer frames are asymmetric** → RESOLVED. Yaw-correct frame 0 to
   bind space (bilateral-symmetry search), then mirror-average L/R bone pairs.
   Binding happens in that symmetric bind space.
3. **Per-model physics changes feel** → MITIGATED. Joint specs (cone/hinge
   limits, friction, motor) are byte-identical; only anchor points move with the
   derived skeleton. Derived centers for the 5 stock models are within ~10 UU of
   canonical, so reach/mass distribution barely shifts. Box remains the gate and
   the fallback for unknown models. Playtest tuning is milestone 2.
4. **Vertex clustering is brittle** → ADDRESSED. Region partitioning uses Z bands
   + `|x|` separation (not free clustering) with per-model thresholds derived
   from the frame bbox; plus hard assertions (every bone ≥ verts, each bone's
   verts inside its box, L/R counts within tolerance) and the Blender render.
5. **Skeleton hands ~11 UU short of mesh hands** → RESOLVED BY CONSTRUCTION. The
   derived forearm center sits at the mesh's actual hand line (z≈-36); the elbow
   hinge anchor moves with it. No arbitrary bone lengthening — the bone follows
   the mesh.
6. **Materials/skins** → RESOLVED. `RagdollCarcass::Initfor` already copies
   `Pawn.MultiSkins[0..3]`; renderer reads `Owner->MultiSkins` per face via
   `UMesh::GetTexture`. Dataset path fills `Mesh->Textures[0..3]` from it.
7. **LOD/scaffolding** → RESOLVED. The dataset path reuses the exact
   `CreatePlayerRagdollMesh` scaffolding (LOD pins, FaceLevel,
   CollapseWedgeThus/PointThus, BuildMeshAdjacency); dataset wedge iVertex values
   are baked (SpecialVerts included) to index the dense vert array directly.
8. **Spawning inside geometry** → RE-CHECK IN PLAYTEST. `ClearCorpseSpawn` lifts
   bodies that spawn embedded in level BSP based on the actual body boxes, so it
   adapts to per-model sizes automatically; verify on DM-Deck16 stairs/ramps.

## Milestones

1. `build_rig.py` + headless-Blender-verified Commando/SelectionMale1 dataset
   (arms confirmed on the arm line, not assumed).
2. Native loader + BuildRagdoll from dataset; DM-Deck16 playtest (DLL first,
   then `ucc make` per AGENTS.md).
3. Scale to all 5 male/female models; fallback rules.
