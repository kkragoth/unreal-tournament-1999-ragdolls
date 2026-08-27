# Epic Games Unreal Tournament Public Source — Notice

This repository intentionally contains **no** files from Epic Games' "Unreal
Tournament Public Source Distribution". To build it you must obtain the Core
and Engine headers plus the prebuilt `Core.lib` / `Engine.lib` import libraries
yourself from the official distribution and place them under
`thirdparty/ut99sdk/` (see the README). Those files are the property of
[Epic Games, Inc](http://www.epicgames.com).

A convenient Git mirror of the distribution is maintained at
[FaultyRAM/Ut99PubSrc](https://github.com/FaultyRAM/Ut99PubSrc). The dev scripts
in `scripts/` (`scripts/Fetch-Ut99Src.ps1`, `scripts/Setup-Dev.ps1`) clone from
that mirror by default — any copy of the original Epic distribution also works
and is covered by the same terms below.

Quoting the original distribution README:

> All files contained here are the property of Epic Games, Inc.
> They are provided without warranty, and under the same terms as the Unreal
> retail license agreement: You may use them for your personal, non-profit
> enjoyment, but you may not sell or otherwise commercially exploit the source
> or things you created based on the source.

This project also requires a legally obtained retail copy of Unreal Tournament
'99 (the runtime DLLs and game content). Nothing in this repository or its
build outputs may be used commercially.
