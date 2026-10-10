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
body at death, default 3.0). Persisted under `[RagdollPkg.RagdollCarcass]`
in UnrealTournament.ini. The 2026-10-10 defaults replaced joint brakes of
1.5-2.4 Nm (15-50% of the limbs' gravity torque, which froze limbs 20-35 deg
from vertical) and ~doubled every joint range. Dataset body boxes are capped
in height and checked for non-adjacent overlap in `build_rig_mixamo.py`
(the spine box used to reach into the thigh boxes and jam the hips).

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
