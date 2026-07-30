# MHW16x10Fix

Steam Deck 16:10 runtime-fix scaffold for Monster Hunter: World / Iceborne.

## Current state

The current `0.4.4-dx11-texture-trace` build is a safe, fail-closed graphics
diagnostic:

- 64-bit `dinput8.dll` proxy that forwards `DirectInput8Create`
- initialization outside `DllMain`
- INI configuration and file logging
- host-process validation
- DX11 hooks for `Present`, `ResizeBuffers`, `CreateTexture2D`,
  `RSSetViewports`, and `RSSetScissorRects`
- descriptor and call-stack logging for resources created at exactly
  `1280x720`
- viewport, render-target, depth-target, scissor, and callsite logging
- no game-memory or viewport modification

The project does **not** yet remove letterboxing. The hooks identify the
creation sites for the game's 720-high scene, post-processing, and depth
resources before any dimensions are changed. The legacy runtime structure
experiment remains in the source tree for research but is not part of the
build.

## Build

Requirements: Visual Studio 2022 with the Desktop development with C++ workload,
and CMake 3.21 or newer.

```powershell
cmake --preset vs2022-x64
cmake --build --preset release
```

Output:

```text
build/Release/dinput8.dll
build/Release/mhw_16x10.ini
```

## Install

Copy both output files beside `MonsterHunterWorld.exe`. In Steam on SteamOS,
use:

```bash
WINEDLLOVERRIDES="dinput8.dll=n,b" %command%
```

If Stracker's Loader already owns `dinput8.dll`, do not overwrite it. A later
plugin build should instead be installed under `nativePC/plugins`.

On failure, remove the custom DLL. Logs are written to `MHW16x10Fix.log`.

## Data needed for implementation

Record the current game version, Iceborne status, DX11/DX12 mode, EXE size and:

```powershell
Get-FileHash .\MonsterHunterWorld.exe -Algorithm SHA256
```

Do not publish saves, Steam credentials, or account data.

The currently inspected executable and static-analysis notes are recorded in
[`docs/supported-builds.md`](docs/supported-builds.md). Being listed there does
not mean a build is patch-supported; the DLL continues to fail closed until
runtime verification is complete.

## Third-party work

The experimental DX11 runtime-structure patch is adapted from the LGPLv3
licensed Lazy Aspect Fix. See
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) and
[`LICENSES/LGPL-3.0.txt`](LICENSES/LGPL-3.0.txt).
