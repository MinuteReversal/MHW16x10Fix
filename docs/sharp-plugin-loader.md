# SharpPluginLoader compatibility

MHW16x10Fix has been tested successfully with SharpPluginLoader 0.0.9 on
Windows and Steam Deck. Both tests used DirectX 12 and confirmed that the game
started normally, SharpPluginLoader initialized, and the native aspect-ratio
fix removed the black bars.

SharpPluginLoader and MHW16x10Fix use different proxy DLL names. Keep
MHW16x10Fix as `dinput8.dll`; do not rename SharpPluginLoader's platform proxy.

## MHW16x10Fix configuration

When using SharpPluginLoader without Stracker's Loader, no chain loader is
needed:

```ini
[Loader]
ChainLoad=
ForwardDirectInput8Create=true
```

`ForwardDirectInput8Create` has no effect while `ChainLoad` is empty.

MHW16x10Fix detects SharpPluginLoader's Windows or Linux package. On DX12 it
then avoids installing its Present hook, because SharpPluginLoader also hooks
the presentation path. Instead, a lightweight startup monitor checks the
game's requested aspect mode every 100 ms for up to 120 seconds and reapplies
native mode only when necessary. The monitor then exits automatically.

## Windows

1. Install the .NET 8 Desktop Runtime required by SharpPluginLoader.
2. Download `SharpPluginLoader-0.0.9-win32.zip` and extract it into the game
   directory beside `MonsterHunterWorld.exe`.
3. Confirm that the game directory contains `winmm.dll` and that
   `nativePC/plugins/CSharp/Loader/SharpPluginLoader.Core.dll` exists.
4. Install MHW16x10Fix normally as `dinput8.dll` with
   `mhw_16x10.ini` beside the executable.
5. Start the game through Steam. No special Windows launch option is needed.

## Steam Deck / Linux through Proton

Use the Linux SharpPluginLoader package. The Windows package's `winmm.dll` is
not the correct bootstrap proxy for this setup.

1. Download `SharpPluginLoader-0.0.9-linux.zip` and extract it into the game
   directory. Version 0.0.9 contains `msvcrt.dll` and the C# Loader files.
2. Remove a previously installed SharpPluginLoader `winmm.dll`, if present.
3. Install Protontricks from Flathub:

   ```bash
   flatpak install flathub com.github.Matoking.protontricks
   ```

4. If the game is installed in an additional Steam library or on an SD card,
   grant Protontricks access to that library. Replace the example path with
   the actual mount point:

   ```bash
   flatpak override --user --filesystem=/run/media/deck/STEAM_LIBRARY com.github.Matoking.protontricks
   ```

   Access to all removable-media mounts can instead be granted with:

   ```bash
   flatpak override --user --filesystem=/run/media com.github.Matoking.protontricks
   ```

5. Install the .NET 8 Desktop Runtime and shader compiler into Monster Hunter:
   World's Proton prefix:

   ```bash
   flatpak run com.github.Matoking.protontricks 582010 dotnetdesktop8 d3dcompiler_47
   ```

   Complete the .NET installers shown by Protontricks, including the x86 and
   x64 components.

6. Set the Steam launch option to the configuration validated on Steam Deck:

   ```bash
   WINEDLLOVERRIDES="dinput8,msvcrt=n,b" %command%
   ```

7. Start the game. A successful MHW16x10Fix log contains lines similar to:

   ```text
   SharpPluginLoader detected; using the DX12 native-aspect worker instead of a Present hook
   Native aspect mode requested: active=1, requested 1 -> 0
   ```

SharpPluginLoader writes its own log to:

```text
nativePC/plugins/CSharp/Loader/SharpPluginLoader.log
```

## Installing Sharp plugins

Put each plugin outside the Loader directory, for example:

```text
nativePC/plugins/CSharp/ExamplePlugin/ExamplePlugin.dll
```

Do not put third-party plugins inside `nativePC/plugins/CSharp/Loader`.

The included compatibility testing covered the empty SharpPluginLoader
framework and a minimal C# lifecycle/UI test plugin on Windows and Steam Deck.
Compatibility with every third-party Sharp plugin cannot be guaranteed.

## Troubleshooting

- No `SharpPluginLoader.log`: SharpPluginLoader did not initialize. Verify the
  correct platform package, Proton dependencies, and DLL overrides.
- Protontricks says the Steam library does not exist: grant its Flatpak access
  to the SD-card or additional-library mount point.
- Windows starts but crashes when both loaders hook DX12 presentation: verify
  that the installed MHW16x10Fix version includes SharpPluginLoader detection
  and that the Sharp Core DLL and platform proxy are present.
- Keep only one case variant of `dinput8.dll` on Linux; do not leave both
  `dinput8.dll` and `DINPUT8.dll` in the game directory.
