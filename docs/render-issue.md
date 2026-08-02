# Renderer Issue: GPF drawing HUD canvas text (source-built D3DDrv on UT v436)

## Status
Documented, unresolved. The game runs fine on the original v436 renderers; this
only affects the renderers rebuilt from this source tree.

## Environment
- Source: Epic Unreal Tournament v432 public source (this repo, CMake-ported), x86 Release build.
- Game: Unreal Tournament v436 install at
  `C:\Users\kkragoth\Downloads\Unreal Tournament\Unreal Tournament`.
- The v432-source renderers are ABI-compatible with the v436 game (the classic supported combo),
  but NOT with the v469 Steam install (see below).

## Symptom
`UnrealTournament.exe` starts, the 3D scene initializes and draws, then a
**general protection fault** occurs on the first frame while the HUD/menu canvas
is drawing text (e.g. "(Press ESC to begin.)").

`UnrealTournament.log` (System dir) crash stack:

```
Critical: URender::DrawFrame
Log: URender::DrawWorld anomaly
Critical: UCanvas::WrappedPrint
Critical: (Press ESC to begin.)
Critical: UCanvas::execDrawText
Critical: (CHNullHUD CityIntro.CHNullHUD0 @ Function Botpack.CHNullHUD.PostRender : 00FB)
Critical: UObject::ProcessEvent
Critical: (TMale1 CityIntro.TMale0, Function Engine.PlayerPawn.PostRender)
Critical: UGameEngine::Draw
Critical: UWindowsViewport::Repaint
Critical: UWindowsClient::Tick
Critical: ClientTick
Critical: UGameEngine::Tick
Critical: UpdateWorld
Critical: MainLoop
```

So the fault is in the canvas text drawing path: script `DrawText` ->
`UCanvas::execDrawText` -> `UCanvas::WrappedPrint` (Render.dll) -> our
`UD3DRenderDevice::DrawTile` / `SetTexture` (Direct3D7.cpp).

## Details
- The 3D world renders (DrawGouraudPolygon path works); only the 2D canvas tile
  path (font glyphs via `DrawTile`, Direct3D7.cpp:1159) crashes.
- Suspect code path: `DrawTile` -> `SetTexture(0, Info, ...)` (Direct3D7.cpp:1169)
  with the 256x256 P8 font texture, then `DrawPrimitiveVB`.
- `UD3DRenderDevice::SetTexture` (Direct3D7.cpp:1854) is the likely fault site;
  it walks the texture cache, creates/updates video textures, and calls
  `Stages[dwStage].MaxColor = ti->MaxColor.Plane();` (Direct3D7.cpp:2063).
- Not yet debugged (no debugger/dump analysis performed).

## Related: source-built OpenGLDrv
- Loads and inits (enumerates all pixel formats with hardware GL), but crashes
  switching to fullscreen: GPF in v436 `UWindowsViewport::ResizeViewport`, called
  from our `SetRes` (OpenGL.cpp:731). So `UseFullscreen`/fullscreen resolution
  change faults; windowed mode was not tried.

## Related: v469 Steam install is NOT compatible
The user's Steam install is v469. The v432-source renderers GPF against it:
- Renderers: GPF in `Engine.dll` `FColor::Plane` (struct layout changed in v469;
  our `FTextureInfo`/`FSurfaceInfo` handling is stale). Source call sites:
  OpenGL.cpp:311 and Direct3D7.cpp:2063.
- `UCC.exe`: v469 `Core.dll` changed `FFileManager::Init` from 1-arg to 2-arg
  (`?Init@FFileManager@@UAEXH@Z` -> `?Init@FFileManager@@UAEXHPBG@Z`), so v432
  UCC fails to load. After patching the header + regenerating `Core.lib` from the
  v469 `Core.dll`, UCC loads but immediately access-violates (object-system ABI
  mismatch). OldUnreal has confirmed they will not release v469 source.

## Verification / workaround
- Original v436 `D3DDrv.dll` and `OpenGlDrv.dll` run the game fine (verified:
  intro -> menu -> DM-Deck16, clean exit).
- Our compiled `UCC.exe` runs on the v436 install (prints banner, exits 0).

## Files changed for this investigation (repo)
- `Core/Inc/Core.h` and `Core/Inc/FFileManagerArc.h` were temporarily edited to the
  v469 `FFileManager::Init` signature, then reverted via `git checkout`.
- `Core/Lib/Core.lib` was replaced with an import lib generated from the v469
  `Core.dll` (original preserved as `Core/Lib/Core.lib.v432.bak`), then restored.
- Standing build fixes (keep): `CMakeLists.txt`, `Setup/Src/Res/SetupRes.rc`,
  `Core/Inc/FFileManagerGeneric.h`.

## Game install state (v436)
- `System/D3DDrv.dll` = original v436 (217088 B); backups of originals kept as
  `*.bak-oursrc` alongside.
- `System/OpenGlDrv.dll` = our source build (73216 B), currently unused.
- `UnrealTournament.ini`: `GameRenderDevice=RenderDevice=D3DDrv.D3DRenderDevice`.
