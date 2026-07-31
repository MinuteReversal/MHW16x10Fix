# MHW16x10Fix

Removes the forced 16:9 letterbox from Monster Hunter: World / Iceborne when
running in DX11, including the Steam Deck's native 1280x800 mode.

## How it works

The game normally requests render aspect mode `1`, which constrains content to
16:9. This proxy waits for the first DX11 Present and calls the game's own
aspect-mode setter with mode `0` (no aspect crop). The engine then rebuilds the
content area at the actual output size.

The fix does not stretch a 720-high image, patch the projection matrix, resize
textures manually, or globally rewrite viewports/scissors.

## Verified configuration

- Monster Hunter: World / Iceborne `15.23.00`
- Steam executable SHA-256:
  `C2EBBBD2C49F216D484E31A5219BED419EB1E5E7D206D02CBA040A3AB79D90EA`
- DX11
- 1280x800
- Windows and Steam Deck
- Steam Deck play session exceeding one hour without a crash

Other 16:10 and ultrawide resolutions may work through the same native mode,
but have not received the same validation.

## Build

Requirements: Visual Studio 2022 with the Desktop development with C++ workload,
and CMake 3.21 or newer.

```powershell
cmake --preset vs2022-x64
cmake --build --preset release
```

The output directory contains `dinput8.dll` and `mhw_16x10.ini`.

## Install

Copy both files beside `MonsterHunterWorld.exe`.

On SteamOS, use:

```bash
WINEDLLOVERRIDES="dinput8.dll=n,b" %command%
```

The game directory can contain only one file named `dinput8.dll`. Another mod
using the same proxy filename will conflict unless a chain-loading solution is
added.

## Configuration

```ini
[Fix]
Enabled=true
AutoDetectAspect=true
Width=1280
Height=800
RemoveLetterbox=true

[Debug]
EnableLog=true
```

With auto-detection enabled, the configured width and height are fallbacks.
Logs are written to `MHW16x10Fix.log`.

## Limitations

- DX12 is not supported.
- A small number of UI dialogs may retain 16:9-oriented positioning.
- Only the listed executable build is supported; the setter bytes are checked
  at runtime and an unknown build fails closed.
