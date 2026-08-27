# Unreal Tournament 99 — Ragdolls

Ragdoll death physics for Unreal Tournament '99 (v436), implemented as a native
Unreal package (`RagdollPkg`) on top of the [Box3D](https://github.com/erincatto/box3d)
physics library.

> **UT99 public source:** the Unreal Tournament '99 public source distribution
> required to build this project is mirrored at
> **[FaultyRAM/Ut99PubSrc](https://github.com/FaultyRAM/Ut99PubSrc)**
> (original Epic Games distribution, personal non-profit use only — see
> [docs/EPIC-source-notice.md](docs/EPIC-source-notice.md)). The dev scripts
> below can clone it for you automatically.

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
tools/                 Python pipeline: extract meshes from .u files -> rig -> datasets header
docs/                  findings, plans, handoff notes from the development sessions
thirdparty/            NOT in the repo - provision locally (see below); gitignored
```

`thirdparty/` holds two local-only dependencies and is deliberately **not
tracked in git** (they are Epic Games' files / an external library):

- `thirdparty/ut99sdk/` - Unreal Engine 1 headers (`Core/Inc`, `Engine/Inc`)
  plus prebuilt import libs (`Core/Lib/Core.lib`, `Engine/Lib/Engine.lib`),
  taken from Epic's UT99 public source distribution.
- `thirdparty/box3d/` - [Box3D](https://github.com/erincatto/box3d) physics
  library sources.

## Setup

### Quick start — auto-fetch UT99 source from GitHub

One command clones [FaultyRAM/Ut99PubSrc](https://github.com/FaultyRAM/Ut99PubSrc),
provisions `thirdparty/`, and configures the build:

```powershell
# clone Ut99PubSrc to thirdparty\_cache\Ut99PubSrc (gitignored), provision + configure
.\scripts\Setup-Dev.ps1 -Configure

# use an existing Ut99PubSrc checkout, or a custom cache location
.\scripts\Setup-Dev.ps1 -Ut99Src "C:\path\to\Ut99PubSrc" -Configure
.\scripts\Setup-Dev.ps1 -Ut99SrcDir "D:\cache\Ut99PubSrc" -Configure

# deploy straight into a retail install's System\ (skips the manual copy step)
.\scripts\Setup-Dev.ps1 -Configure -GameSystemDir "C:\Unreal Tournament\System"
```

What it does: [`scripts/Setup-Dev.ps1`](scripts/Setup-Dev.ps1) calls
[`scripts/Fetch-Ut99Src.ps1`](scripts/Fetch-Ut99Src.ps1) to `git clone` /
`git pull` the public source if you did not pass `-Ut99Src`, then forwards to
`setup.ps1` to populate `thirdparty/ut99sdk` and `thirdparty/box3d`.

To only fetch/update the source without provisioning:

```powershell
.\scripts\Fetch-Ut99Src.ps1                # -> thirdparty\_cache\Ut99PubSrc
.\scripts\Fetch-Ut99Src.ps1 -Destination "C:\dev\Ut99PubSrc" -Branch master
```

`Fetch-Ut99Src.ps1` is idempotent — re-run it to `git pull` the latest source.

### Manual setup — existing UT99 source checkout

If you already have the UT99 public source tree locally (from
[FaultyRAM/Ut99PubSrc](https://github.com/FaultyRAM/Ut99PubSrc) or elsewhere),
provision both dependencies in one step with the bundled script:

```powershell
.\setup.ps1 -Ut99Src "C:\path\to\ut99-public-source"
```

- `-Ut99Src` - path to your UT99 public source distribution tree (must contain
  `Core\`, `Engine\` incl. the prebuilt `Core\Lib\Core.lib` / `Engine\Lib\Engine.lib`).
  Any clone of https://github.com/FaultyRAM/Ut99PubSrc satisfies this.
- `-Box3dSrc <path>` - optional: copy Box3D from an existing checkout instead of
  cloning from GitHub.
- `-Configure` - also run the CMake configure afterwards.

The script is idempotent; re-run it to refresh either dependency.

Box3D version note: this project was tested against upstream commit `67c92ab`
("Fix b3PopCount64 for 32-bit x86 targets" - required for the Win32 build).
Upstream rewrote its history, so that exact ref may not be fetchable anymore;
the script then falls back to upstream's default branch with a warning, and
newer Box3D (SIMD hulls, hash rework) remains untested here. If you have a
checkout of the tested commit, pass it via `-Box3dSrc`.

## Prerequisites

- Visual Studio 2019/2022 with the **x86 (Win32)** MSVC toolset
- CMake >= 3.7
- A retail Unreal Tournament installation (provides `Core.dll`, `Engine.dll`,
  `ucc.exe` and the game content at runtime)
- Python 3 for the tooling (Blender optional, only for rig verification renders)
- The UT99 public source distribution — **no manual download required**:
  `scripts/Setup-Dev.ps1` auto-clones https://github.com/FaultyRAM/Ut99PubSrc
  on first run (or pass an existing checkout via `-Ut99Src` / `setup.ps1 -Ut99Src`)

## Build

```powershell
# one-time thirdparty provisioning (see Setup above) — either:
.\scripts\Setup-Dev.ps1 -Configure                          # auto-fetch Ut99PubSrc from GitHub
# or
.\setup.ps1 -Ut99Src "C:\path\to\ut99-public-source" -Configure  # existing checkout

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
del RagdollPkg.u
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

## Configuration

`RagdollPkg.RagdollCarcass` exposes one persisted flag:

| Flag | Default | Description |
|------|---------|-------------|
| `bBoxMannequins` | `false` | When `true`, corpses use the procedural box mannequin (full 14-body skeleton incl. feet, no mesh skinning) even for models that have a real-mesh dataset. When `false` (default), models with a dataset get the real skinned player mesh on a 12-body skeleton; unknown models always fall back to boxes. |

Toggle at runtime from the console:

```console
set RagdollPkg.RagdollCarcass bBoxMannequins 1   # box mannequins
set RagdollPkg.RagdollCarcass bBoxMannequins 0   # real skinned meshes
```

or persistently in `<retail>\System\UnrealTournament.ini`:

```ini
[RagdollPkg.RagdollCarcass]
bBoxMannequins=True
```

The change applies to corpses spawned afterwards; corpses already lying around keep their current skeleton. No recompile needed either way.

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

This repository does **not** distribute any Epic Games source, headers, or
libraries. Building requires the UT99 public source distribution's Core/Engine
headers and import libraries (provisioned into `thirdparty/ut99sdk/`), which
remain property of Epic Games under the terms described in
[docs/EPIC-source-notice.md](docs/EPIC-source-notice.md): personal, non-profit
use only. A convenient Git mirror of that distribution is maintained at
[FaultyRAM/Ut99PubSrc](https://github.com/FaultyRAM/Ut99PubSrc) — the dev
scripts in [`scripts/`](scripts/) clone from there by default, but any copy of
the original Epic distribution works. Box3D is MIT licensed, upstream at
[erincatto/box3d](https://github.com/erincatto/box3d), and is likewise not
included in this repo. Everything committed here (the `RagdollPkg` package,
tooling, docs) follows the same personal, non-profit terms.
