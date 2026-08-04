# MHW16x10Fix

An aspect ratio fix and black bar remover for Monster Hunter: World /
Iceborne. It removes the forced 16:9 letterbox (top and bottom black bars) and
enables native 16:10 rendering, including the Steam Deck's 1280x800 display.
Both DX11 and DX12 are supported.

Useful search terms: MHW aspect ratio fix, remove black bars, letterbox remover,
16:10 fix, 1280x800 fix, Steam Deck mod, widescreen fix, DX11, and DX12.

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
- SharpPluginLoader 0.0.9 on Windows and Steam Deck (DX12)

Other 16:10 and ultrawide resolutions may work through the same native mode,
but have not received the same validation.

DX12 uses the same native engine setter and has been visually and internally
validated on Windows and Steam Deck at 1280x800. SharpPluginLoader
compatibility was also validated on both platforms. See the
[SharpPluginLoader compatibility guide](docs/sharp-plugin-loader.md) for the
required platform packages, Proton dependencies, and launch options.

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

When SharpPluginLoader is also installed on Steam Deck, use its Linux package
and the separately documented launch option instead. See
[SharpPluginLoader compatibility](docs/sharp-plugin-loader.md).

The game directory can contain only one file named `dinput8.dll`. To use
another proxy mod at the same time:

1. Keep this project's DLL named `dinput8.dll`.
2. Rename the other mod's proxy DLL to `dinput8_chain.dll`.
3. Put both DLLs beside `MonsterHunterWorld.exe`.
4. Set `ChainLoad=dinput8_chain.dll` in `mhw_16x10.ini`.

Stracker's Loader must not receive forwarded `DirectInput8Create` calls after
its proxy DLL is renamed, because its own forwarding path resolves back to the
top-level `dinput8.dll`. For Stracker's Loader, also set:

```ini
ForwardDirectInput8Create=false
```

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
ForwardDirectInput8Create=true

[Debug]
EnableLog=true
```

With auto-detection enabled, the configured width and height are fallbacks.
Logs are written to `MHW16x10Fix.log`.

When chain loading is configured, the downstream DLL is loaded before the
first DirectInput call is completed. With `ForwardDirectInput8Create=true`, a
downstream `DirectInput8Create` export receives the call. With the option set
to `false`, the downstream DLL still receives its normal `DllMain`
initialization while DirectInput calls continue directly to the Windows system
DLL. The latter setting avoids recursive forwarding with Stracker's Loader.
The default remains `true` for compatibility with other proxy DLLs. The log
records which case occurred. Self-loading is rejected.

## Compatibility guides

- [SharpPluginLoader on Windows and Steam Deck](docs/sharp-plugin-loader.md)

## Limitations

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
