# Unreal Tournament 99 — Ragdolls

Ragdoll death physics for Unreal Tournament '99 (v436), implemented as a native
Unreal package (`RagdollPkg`) on top of the [Box3D](https://github.com/erincatto/box3d)
physics library.

- Corpses are 12-body articulated rigid bodies (pelvis/spine/neck/head, arms,
  legs; feet skipped and bound to shins) with hinge joints and per-bone damage
  impulses — shooting a limb shoves it.
- The player model's real mesh is skinned onto the skeleton: each Selection
  model gets a generated dataset (bone boxes + joint anchors + per-vertex bone
  bind) emitted by `tools/build_rig.py` into `RagdollPkg/Src/RagdollDatasets.h`.
  Models without a dataset fall back to box mannequins.
- Corpses collide with the level's real BSP geometry: the map's solid surfaces
  are baked into one shared static Box3D world per level.

## Repository layout

```
RagdollPkg/            the UT package: native C++ (Src/, Inc/) + UnrealScript (Classes/)
thirdparty/box3d       Box3D physics library, vendored snapshot (see VENDORED.md there)
thirdparty/ut99sdk/    minimal UE1 SDK subset: Core/Engine headers + prebuilt import libs
tools/                 Python pipeline: extract meshes from .u files -> rig -> datasets header
docs/                  findings, plans, handoff notes from the development sessions
```

## Prerequisites

- Visual Studio 2019/2022 with the **x86 (Win32)** MSVC toolset
- CMake >= 3.7
- A retail Unreal Tournament installation (provides `Core.dll`, `Engine.dll`,
  `ucc.exe` and the game content at runtime)
- Python 3 for the tooling (Blender optional, only for rig verification renders)

## Build

```powershell
cmake -S . -B build -A Win32
cmake --build build --config Release --target RagdollPkg
```

The DLL is written to `System\` in this repo (or straight into the retail
install if you configured `-DGAME_SYSTEM_DIR="C:\...\Unreal Tournament\System"`).

Deploy manually with:

```powershell
copy System\RagdollPkg.dll "<retail>\System\"
```

## UnrealScript build

`.uc` classes compile via the game's own compiler. From the retail `System\`
directory:

```powershell
# ucc make skips recompilation if RagdollPkg.u exists - delete it first
del ..\System\RagdollPkg.u   # actually: del RagdollPkg.u
ucc make RagdollPkg
```

Notes:

- New native exec functions must be built into `RagdollPkg.dll` **before**
  running `ucc make`, or it fails with "Can't find ... in RagdollPkg.dll".
- If asked "RagdollPkgClasses.h needs to be updated (Y/N)", answer **N**:
  `RagdollPkg/Inc/RagdollPkgClasses.h` is hand-maintained with `-1` native
  indices matched by name at runtime.

For `ucc make` to see this checkout's sources, either copy the tree or make the
retail install reference it, e.g. a directory junction:

```powershell
New-Item -ItemType Junction -Path "<retail>\RagdollPkg" -Target "<repo>\RagdollPkg"
```

and register the package for compilation by adding an `EditPackages=RagdollPkg`
line under the `[Editor]` section of `<retail>\System\UnrealTournament.ini`.

## Playtest

From the retail `System\`:

```powershell
.\UnrealTournament.exe "DM-Deck16][?game=Botpack.DeathMatchPlus?MinPlayers=8?mutator=RagdollPkg.RagdollMutator,RagdollPkg.RagdollTestMutator" -log
```

Logs land in `<retail>\System\UnrealTournament.log`.

## Tooling

```powershell
# Regenerate RagdollDatasets.h (+ OBJ/PNG overlays in tools/rig_verify/)
$env:BLENDER_EXE="C:\Program Files\Blender Foundation\Blender 3.6\blender.exe"
python tools/build_rig.py "<retail>\System\Botpack.u" --obj-dir tools/rig_verify --render

# Mesh/package inspection utilities
python tools/pkg_scan.py <file.u>
python tools/extract_mesh.py <file.u> <ClassName>
```

See `docs/handoff.md` for the current work state and `docs/skinning-findings.md`
for how the mesh binding works before touching skinning code.

## Attribution / license

Built against Epic Games' Unreal Tournament public source distribution headers
and import libraries. Those files remain property of Epic Games and are provided
under the terms described in [docs/EPIC-source-notice.md](docs/EPIC-source-notice.md):
personal, non-profit use only. Everything else in this repo (the `RagdollPkg`
package, tooling, docs) follows the same terms.
