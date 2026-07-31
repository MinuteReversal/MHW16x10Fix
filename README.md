# MHW16x10Fix

Removes the forced 16:9 letterbox from Monster Hunter: World / Iceborne,
including the Steam Deck's native 1280x800 mode. Both DX11 and DX12 are
supported.

## How it works

The game normally requests render aspect mode `1`, which constrains content to
16:9. This proxy waits for the first graphics Present and calls the game's own
aspect-mode setter with mode `0` (no aspect crop). The engine then rebuilds the
content area at the actual output size.

The fix does not stretch a 720-high image, patch the projection matrix, resize
textures manually, or globally rewrite viewports/scissors.

## Verified configuration

- Monster Hunter: World / Iceborne `15.23.00`
- Steam executable SHA-256:
  `C2EBBBD2C49F216D484E31A5219BED419EB1E5E7D206D02CBA040A3AB79D90EA`
- DX11
- DX12
- 1280x800
- Windows and Steam Deck
- Steam Deck play session exceeding one hour without a crash

Other 16:10 and ultrawide resolutions may work through the same native mode,
but have not received the same validation.

DX12 uses the same native engine setter and has been visually and internally
validated on Windows at 1280x800. Extended DX12 stability testing on Steam
Deck remains pending.

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

The game directory can contain only one file named `dinput8.dll`. To use
another proxy mod at the same time:

1. Keep this project's DLL named `dinput8.dll`.
2. Rename the other mod's proxy DLL to `dinput8_chain.dll`.
3. Put both DLLs beside `MonsterHunterWorld.exe`.
4. Set `ChainLoad=dinput8_chain.dll` in `mhw_16x10.ini`.

The configured path may also be absolute. Relative paths are resolved from the
game directory.

## Configuration

```ini
[Fix]
Enabled=true
AutoDetectAspect=true
Width=1280
Height=800
RemoveLetterbox=true

[Loader]
ChainLoad=

[Debug]
EnableLog=true
```

With auto-detection enabled, the configured width and height are fallbacks.
Logs are written to `MHW16x10Fix.log`.

When chain loading is configured, the downstream DLL is loaded before the
first DirectInput call is completed. If it exports `DirectInput8Create`, the
call is forwarded to it; otherwise the DLL still receives its normal
`DllMain` initialization and DirectInput calls continue to the Windows system
DLL. The log records which case occurred. Self-loading is rejected.

## Limitations

- Extended DX12 stability testing on Steam Deck is still pending.
- A small number of UI dialogs may retain 16:9-oriented positioning.
- Only the listed executable build is supported; the setter bytes are checked
  at runtime and an unknown build fails closed.
- Chain loading solves the common single-proxy filename conflict. It cannot
  guarantee compatibility between mods that hook or patch the same game or
  graphics functions, and a downstream proxy with its own hard-coded filename
  assumptions may still require that mod's documented loader arrangement.

## Source and license

Source code: <https://github.com/MinuteReversal/MHW16x10Fix>

MHW16x10Fix is original work developed through runtime analysis and controlled
testing. REFramework was consulted only as a high-level architectural reference
for graphics API hooking; no REFramework source is included.

Licensed under the [MIT License](LICENSE). You may use, modify, and redistribute
the project, including commercially, provided the copyright and license notice
are retained.
