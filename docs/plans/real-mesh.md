# Plan: real-mesh skinning via external extraction + rig

Status: **PLANNING** (not started). Supersedes the runtime frame-scan approach that
was reverted. Read `docs-skinning-findings.md` first — it records every prior
experiment and the fundamental mismatch this plan works around.

## The core problem (from findings)

- The Box3D physics skeleton MUST spawn in a T-pose (limbs spread). Bodies that
  spawn interpenetrating explode at spawn time.
- A freshly killed player's real mesh is in a standing pose (arms at sides).
- Skinning that standing mesh onto the spread T-pose skeleton forces
  nearest-bone assignment to glue arms to the torso (arm bones starve to 0 verts,
  torso bones swallow everything) and the verts tear when the bodies swing.
- No stock animation frame (dead or otherwise) gives a full spread:
  `Dead2 f=518` best-ever = `{17,81,39,140,25,15,21,12,1,0,0,0,0,0}` — legs 0.

## The strategy

Do the mesh re-pose and vertex->bone skinning **offline, externally**, instead of
at runtime:

1. Extract the player LODMesh from the `.u` package with a standalone parser.
2. **Synthesize a T-pose vertex frame** by rotating limb vertex groups about the
   mesh's real joints. No stock frame provides it, so we construct it.
3. Assign verts to bones in the T-pose (so arm verts sit next to arm bones),
   compute per-vertex bind offsets in bone-local space.
4. Verify externally with a small renderer (standing mesh, T-pose + skeleton
   overlay, a few deformed poses).
5. Runtime just loads the precomputed bind data and emits `boneWorld * offset`
   — the same math the box mannequin already uses, with real verts. Because the
   bind pose == physics spawn pose (T-pose), there is no spawn tear and no
   runtime frame scanning / CPU burn.

## Ground truth to cross-check the parser against

All from `docs-skinning-findings.md` / prior sessions:

- ~700 animation frames total.
- Sequence table (`FMeshAnimSeq` in `Engine\Inc\UnAnim.h:34-46`):
  `All`, `Dead1[525+13]`, `Dead2[538+16]`, `Dead3[554+13]`, `Dead4[567+16]`,
  `Dead7[583+21]`, `Dead8[604+18]`, `Dead9[622+20]`, `Dead9B[642+10]`,
  `Dead11[652+18]` (format: `name[startFrame+numFrames]`).
- Mesh bounds: `(-1500..2432, -2108..2048, -1596..708)` UU.
- Mesh import transform (from `CreatePlayerRagdollMesh`):
  `(vert - Origin)` rotated by `RotOrigin`, scaled by mesh `Scale` and actor
  `DrawScale` -> actor-local space (pelvis at origin, Z up, arms along +-X),
  matching `GRagdollBoneDefs`.
- Physics skeleton: `RAGDOLL_U2M = 0.02`; `GRagdollBoneDefs`
  (`RagdollCarcass.cpp:44-60`): Pelvis(0,0,0), Spine(0,0,16), Neck(0,0,25),
  Head(0,0,33), UpperArmL/R(+-13,6,24), ForearmL/R(+-13,8,12),
  ThighL/R(+-7,0,-12), ShinL/R(+-7,0,-30), FootL/R(+-7,0,-37).
  `RAGDOLL_BONE_COUNT = 14`.

## Steps

### 0. Identify the mesh asset
- Find which package/export the mod's player mesh comes from (log
  `C->Mesh` package + export name once from the running game, or scan retail
  `.u` files for the LODMesh whose sequence table matches the ground truth).
- Confirm: frame count, sequence table, bounds.

### 1. `tools/extract_mesh.py` — standalone package parser
- Parse UT99 `.u` package format (header, name table, import/export tables,
  zlib-compressed export data). Pure Python 3.9, no engine needed.
- Deserialize the LODMesh/ULodModel: face/tri index list, UVs, per-frame vertex
  arrays, sequence table, import transform (`Origin`/`RotOrigin`/`Scale`).
- Output `.npz` + per-frame OBJ (for eyeballing in an external viewer).

### 2. Verify the parser
- Compare parsed frame count, sequence names/start/num, and bounds against the
  ground truth above. Do not proceed until these match exactly.

### 3. `tools/build_rig.py` — synthesize T-pose + skin
- Place the 14 bones from `GRagdollBoneDefs` into mesh space using the import
  transform (pelvis at origin, Z up).
- Segment verts into limb groups (nearest-bone on standing `All` frame 0).
- Synthesize a T-pose vertex frame: rotate each limb group about the mesh's
  joint pivots (shoulder/elbow/hip/knee) into the spread pose. Joint pivots are
  derived from `GRagdollBoneDefs` and refined visually.
- Re-assign verts -> bones in the T-pose, compute per-vertex bind offsets in
  bone-local frame. Assert every bone has verts (no starvation).
- Emit skin dataset: bind verts, faces, UVs, per-vertex `{boneIndex, offset}`
  (bone-local), bone rest transforms.

### 4. `tools/render_mesh.py` — external verification
- Minimal software renderer (or OBJ -> image). Render and visually confirm:
  - (a) standing mesh matches in-game model,
  - (b) T-pose + 14-bone skeleton overlay is anatomically sane,
  - (c) a few bone-transformed poses deform without tearing.
- Iterate here, not in the game. Fast loop.

### 5. Runtime integration
- Embed the skin dataset (static array header, e.g.
  `RagdollPkg\Inc\RagdollMeshData.h`).
- Re-enable real-mesh path: set `RAGDOLL_USE_REAL_MESH` = 1 in
  `RagdollPkg\Src\UnRagdollMesh.cpp`; new `CreatePlayerRagdollMesh` builds the
  mesh from bind verts instead of the box; `GetFrame` emits
  `boneWorldTransform * bindOffset` for each vertex.
- Physics spawns T-pose exactly as it does now -> mesh matches at spawn, no tear.
- Keep the box mannequin as a fallback flag.

### 6. Playtest
- Build/deploy/playtest on DM-Deck16 per AGENTS.md (DLL first, then `ucc make`).
- Confirm corpse = real player model in spread ragdoll, limbs follow the sim.
- Rig issues get fixed externally (tools), then the header is regenerated —
  no in-game iteration needed.

## Deliverables

- `tools/extract_mesh.py`, `tools/build_rig.py`, `tools/render_mesh.py`
- Skin dataset (`.npz` / generated `RagdollPkg\Inc\RagdollMeshData.h`)
- Updated `docs-skinning-findings.md` with the external approach's outcome.

## Out of scope (for now)

- Smooth multi-bone weights (rigid 1-bone skinning first, matches box path).
- Per-corpse unique mesh (all corpses share one precomputed skin for the base
  player model; skin customization later).
- Any runtime frame scanning.

## GLog constraint (critical)

Any runtime logging must stay well under 1000 chars per line — `TCHAR Ch[1024]`
in `Core\Inc\FOutputDeviceFile.h:69-71` overflows to AV `0xc0000005` /
fail-fast `0xc0000409`.
