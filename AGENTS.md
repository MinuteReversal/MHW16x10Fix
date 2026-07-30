# MHW16x10Fix agent handoff

## Goal

Remove the 16:9 letterboxing from Monster Hunter: World at 1280x800 on
Steam Deck, while keeping the game stable under Proton.

## Paths and target build

- Project: `E:\repository\MHW16x10Fix`
- Windows test installation:
  `E:\SteamLibrary\steamapps\common\Monster Hunter World`
- Log: `<game directory>\MHW16x10Fix.log`
- Target executable version: Monster Hunter: World 15.23.00
- Target EXE SHA-256:
  `C2EBBBD2C49F216D484E31A5219BED419EB1E5E7D206D02CBA040A3AB79D90EA`
- Target configuration: 1280x800, borderless window, DX11, DLSS off,
  in-game aspect preset 16:9.
- SteamOS launch option:
  `WINEDLLOVERRIDES="dinput8.dll=n,b" %command%`
- Windows testing does not need a launch option.

## Current implementation

- The project builds a 64-bit `dinput8.dll` proxy with C++20/CMake/Ninja.
- System `dinput8.dll` exports are forwarded and DLL loading/logging work.
- Active installed version at the time of this handoff:
  `0.4.3-dx11-target-trace`.
- Current code is diagnostic only. It does not alter game memory, viewports,
  textures, or presentation.
- `src/d3d11_diagnostics.cpp` hooks:
  - `IDXGISwapChain::Present`
  - `IDXGISwapChain::ResizeBuffers`
  - the real immediate context's `RSSetViewports`
  - the real immediate context's `RSSetScissorRects`
- For 1280x720 and 1280x800 viewports, version 0.4.3 also logs the bound
  RTV/DSV texture dimensions and an eight-frame call stack.

## Verified rendering facts

- The real DX11 swap chain is 1280x800, format 87, single-sampled.
- The game renders its scene through actual 1280x720 resources and later
  outputs to the 1280x800 swap chain. The bars are not merely introduced by
  Windows or the display.
- Captured 1280x720 resources include:
  - RTV format 41, no DSV
  - RTV format 27, no DSV
  - RTV format 26 with DSV format 39
  - DSV format 39 with no RTV in depth-only passes
- The 1280x800 render target is the swap-chain target, format 87.
- The common game wrapper callsite for `RSSetViewports` is:
  `MonsterHunterWorld.exe+0x259B359`.
- The common wrapper callsite for `RSSetScissorRects` is:
  `MonsterHunterWorld.exe+0x259B425`.
- Useful callers above the viewport wrapper:
  - `+0x25999FB`: 1280x720 RTV format 41
  - `+0x2598F65`: 1280x720 RTV format 27 and the 1280x800 output target
  - `+0x25992F5`: 1280x720 depth-only DSV format 39
  - `+0x2599065`: 1280x720 RTV format 26 / DSV format 39 and depth-only use
- Shared higher stack:
  `+0x229CEC1`, `+0x229AC31`.
- Numerous HUD scissor rectangles use bottom=720. This confirms the UI safe
  area/layout also remains 16:9 and may require a separate adjustment after
  scene rendering is expanded.
- No viewport with `TopLeftY=40` was observed. The 40-pixel top and bottom
  bars result from the 720-high render pipeline being presented within an
  800-high output.

## Approaches that must not be repeated

- Do not blindly rewrite every 1280x720 viewport to 1280x800. The bound
  textures are still 720 pixels high, so this can clip, corrupt rendering, or
  write outside the intended render area.
- Do not return to the old broad writable-memory scan with fixed derived
  offsets. Version `0.3.1-dx11-experimental` found an old render structure and
  wrote offsets derived from Lazy Aspect Fix (`+0x23D90` and `+0x78`), which
  caused a crash on this game build.
- The crashing DLL was preserved, disabled, as:
  `<game directory>\dinput8.dll.0.3.1-crash-disabled`.
- Do not compile or replace the DLL while the game is running. It causes
  severe stutter for the user. Read-only log inspection while running is OK.
- Before deployment, verify `MonsterHunterWorld.exe` is no longer running.
  Preserve the currently installed DLL as a versioned backup and compare
  SHA-256 hashes after copying.

## Recommended next step

Continue on the REFramework-style graphics API hook route:

1. Hook the real `ID3D11Device::CreateTexture2D`.
2. Initially make this diagnostic-only.
3. Log only texture descriptions whose dimensions are exactly 1280x720,
   including format, bind flags, usage, mip/array/sample information, and a
   short call stack/module offsets.
4. Use those creation callsites and descriptors to distinguish the scene
   color, post-processing, and depth resources from unrelated 720p textures.
5. Only after the complete resource set is identified, create an experimental
   opt-in build that changes the selected 1280x720 render/depth resources to
   1280x800 and changes their matching viewports.
6. Keep a fast configuration switch or separate diagnostic build so the
   modification can be disabled if it crashes.
7. Address HUD/scissor/safe-area layout separately after the 3D scene renders
   at 16:10.

Hook the real device obtained from the swap chain, as already done for the
real immediate context; do not assume the dummy D3D11 device is the game's
device.

## Build and test notes

- Typical build environment is Visual Studio 2022 Community `vcvars64.bat`
  followed by:
  `cmake --build build-ninja --config Release`
- Output: `build-ninja\dinput8.dll`
- Always test first on Windows at 1280x800/DX11. Final validation must also be
  performed on Steam Deck because a 16:9 monitor cannot visually demonstrate
  the Deck's 16:10 behavior reliably.
- The user reports game state explicitly ("started", "entered scene",
  "exited"). Wait for an explicit exit before compiling/deploying.

## Reference work and licensing

- Architectural reference:
  `https://github.com/praydog/REFramework`
- Lazy Aspect Fix source was cloned for study at:
  `C:\Users\zy336\Documents\Codex\2026-07-30\new-chat\work\lazy-aspect-fix`
- Lazy Aspect Fix is LGPLv3. The project already includes
  `THIRD_PARTY_NOTICES.md` and `LICENSES\LGPL-3.0.txt`.
- REFramework supports RE Engine titles rather than MHW's MT Framework, so use
  its hooking architecture as guidance, not its game-specific offsets.
