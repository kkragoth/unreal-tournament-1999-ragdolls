# Mixamo rig workflow — handoff (SUPERSEDED 2026-10-10)

> Superseded by `docs/tpose-rig-workflow.md`: Mixamo bound the hanging hands
> to the leg bones ("hands stitched to legs"). The datasets are now built
> from `tools/rigs/*.json` (in-house T-pose + Blender bone-heat rig). Kept
> for the FBX/JSON format notes and the gotchas below.

Status date: 2026-08-26. Read this first, then `docs/handoff.md` + AGENTS.md.
This supersedes the topology-segmentation plan (`docs/plans/topology-rig.md`)
for the binding: **Mixamo's auto-rig skin weights are the binding source.**

## Where things stand (TL;DR)

- Real-mesh corpses driven by **Mixamo-rigged datasets** now render in-game and
  look good. Verified by playtest with ALL 5 model entries temporarily pointed
  at the single rigged `SelectionMale1` dataset.
- The old per-model geometric binding (`tools/build_rig.py` `derive_bones` /
  topo segmentation) is **not** what the new datasets use; it's only used as a
  fallback when regenerating the header for un-rigged models.
- Mixamo upload format that works: **OBJ** (yaw-corrected front-facing,
  complete mesh, WITH UVs). FBX upload FAILS on Mixamo ("Unknown error while
  generating motion"), OBJ works.
- `bBoxMannequins` defaults `false` (real skinned meshes default); ini already
  has `bBoxMannequins=False`. Don't force boxes.

## The one critical gotcha (already hit)

- The dataset material flag MUST be `PF_TwoSided = 0x00000100`.
  `0x00000001` is `PF_Invisible` — corpses render invisible (the "ragdolls
  disappear" bug). Fixed in `build_rig_mixamo.py` and the header.

## The per-model pipeline (one model at a time)

1. **Export the upload OBJ** (already done for all 5; files exist):
   ```
   python tools/export_mixamo_fbx.py "<retail>\System\Botpack.u" --model SelectionMale2 --obj-dir tools/rig_verify
   ```
   Writes `tools/rig_verify/<Model>_mixamo.obj` — yaw-corrected front-facing,
   complete 642-face mesh (NOT weld-dropped), one position + one UV per source
   wedge. Do NOT regenerate unless rig logic changed.

2. **User uploads the OBJ to mixamo.com** → auto-rig → **downloads the T-Pose
   FBX** (e.g. `C:\Users\kkragoth\Downloads\T-Pose (2).fbx`).

3. **Dump the FBX to JSON** (Blender 3.6 at
   `C:\Program Files\Blender Foundation\Blender 3.6\blender.exe`):
   ```
   & "C:\Program Files\Blender Foundation\Blender 3.6\blender.exe" -b --python-exit-code 1 `
     -P tools/fbx_to_json.py -- "<fbx>" "<tmp>\<Model>.json"
   ```

4. **Build the dataset + regenerate the header**:
   ```
   python tools/build_rig_mixamo.py "<tmp>\<Model>.json" `
     --live Soldier --sel SelectionMale2 `
     --header RagdollPkg/Src/RagdollDatasets.h
   ```
   - `--live` is the in-game pawn mesh name; `--sel` the Selection mesh name.
   - The `--header` path rebuilds the OTHER 4 entries with the **legacy**
     pipeline (`bind_mode="legacy"`) and puts the Mixamo entry first. (Topo
     fails on some models — don't use it here.)
   - Verify the printed skeleton summary + per-bone counts, then check the
     header table line for the new model now references `G<Sel>_Verts`.

5. **Rebuild DLL + deploy + playtest**:
   ```
   & "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
     build\RagdollPkg\RagdollPkg.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:BuildProjectReferences=false /v:minimal /nologo
   copy System\RagdollPkg.dll "<retail>\System\"
   <retail>\System\UnrealTournament.exe "DM-Deck16][?game=Botpack.DeathMatchPlus?MinPlayers=8?mutator=RagdollPkg.RagdollMutator,RagdollPkg.RagdollTestMutator -log"
   ```
   (from the retail `System\`; kill any running UnrealTournament first).
   Logs land in `<retail>\System\UnrealTournament.log` — the game locks the
   file exclusively while running; read it only after quitting, or via a
   read-share stream (`.Open(..., Read, ReadWrite)`).

## Model mapping (live pawn mesh ↔ Selection mesh ↔ upload OBJ)

| live name | Selection mesh | upload OBJ              | status          |
|-----------|----------------|-------------------------|-----------------|
| Commando  | SelectionMale1 | SelectionMale1_mixamo.obj | RIGGED (this run) |
| Soldier   | SelectionMale2 | SelectionMale2_mixamo.obj | not yet        |
| Boss      | SelectionBoss  | SelectionBoss_mixamo.obj  | not yet        |
| SGirl     | SelectionFemale2 | SelectionFemale2_mixamo.obj | not yet      |
| FCommando | SelectionFemale1 | SelectionFemale1_mixamo.obj | not yet      |

Current header (`RagdollPkg/Src/RagdollDatasets.h`) has ALL 5 entries pointing
at the SelectionMale1 Mixamo dataset (test staging, `DatasetName` says
SelectionMale1 for all). As each new model is rigged, the table entry is
replaced by its own `G<Sel>_*` arrays.

## Tools (active repo `tools/`)

- `export_mixamo_fbx.py` — yaw-corrected front-facing OBJ with UVs for Mixamo
  (complete mesh, 642 faces). Uses `build_rig(... bind_mode="legacy")` then
  takes `m.faces` (complete) + own wedge remap.
- `fbx_to_json.py` — headless Blender FBX→JSON dumper (verts/faces/UVs/bones/
  weights). Must run under Blender's python, not system python.
- `build_rig_mixamo.py` — FBX-JSON → ragdoll dataset. Maps the 41 Mixamo bones
  to our 14 (finger/toe→parent; feet→shins), derives:
  - centers/halves from the **weighted vertex clusters** (mirror-averaged L/R,
    capped like the old pipeline), NOT skeleton midpoints — boxes sit on the
    actual mesh;
  - 13 joint anchors from the **Mixamo skeleton joint heads** (shoulder/elbow/
    hip/knee/ankle/neck/head);
  - binds = top-3 Mixamo skin weights re-indexed;
  - skull verts: Mixamo weights the whole low-poly head to `Neck`, so the
    cluster pass merges top torso verts into the Head cluster so the Head body
    owns its skull.
- `export_tpose.py` — **BROKEN, do not use.** Rotated the feet (|x|>11
  classifier grabs wide feet) producing a nonsensical mesh. Superseded by
  `export_mixamo_fbx.py` (the "original mesh, don't re-pose" requirement means
  upload the yaw-corrected FRONT-FACING mesh, not a hand-made T-pose).
- `build_rig.py` — modified: the return dict now includes `"report"` (the
  derive report) used by the exporters. Otherwise unchanged.

## Known caveats / notes

- **Hands→feet weights**: with the arms-down bind pose, Mixamo weights the
  hanging hands toward the foot bones. On SelectionMale1 it was acceptable
  enough to look good in-game; if a model's corpse shows hands glued to shins,
  that's why. The proper fix is a true T-pose input (re-pose in Blender), but
  that was shelved — user approved the current look.
- The FBX mesh verts matched our bind verts to 0.00 UU (Blender FBX import
  applies the same transform), so the dataset is in the same bind space.
- FBX export from Blender of our OBJ (for upload) FAILS Mixamo; upload the OBJ
  directly.
- Header is generated (`DO NOT EDIT` header comment); the ONLY hand-edit so far
  is the all-5→SelectionMale1 test staging in `GRagdollDatasetTable`. Next
  header regeneration replaces it.
- Retail dir: `C:\Users\kkragoth\Downloads\Unreal Tournament\Unreal Tournament\`
  (its `RagdollPkg\` is a junction into the active repo).

## Uncommitted working-tree files (do not lose)

Modified: `RagdollPkg/Src/RagdollDatasets.h`, `docs/handoff.md`,
`tools/build_rig.py`.
Untracked: `tools/build_rig_mixamo.py`, `tools/export_mixamo_fbx.py`,
`tools/export_tpose.py`, `tools/fbx_to_json.py`,
`docs/plans/topology-rig.md`.

## Next steps (when resuming)

1. Ask user for the next downloaded T-Pose FBX path (Soldier/SelectionMale2
   next), run pipeline steps 3-5.
2. Repeat for Boss, SGirl, FCommando.
3. Optionally commit the tooling + first rigged dataset once 1+ models done.
