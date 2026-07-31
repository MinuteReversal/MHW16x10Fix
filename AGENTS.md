# MHW16x10Fix agent handoff

## Goal

Remove the 16:9 letterboxing from Monster Hunter: World at 1280x800 on
Steam Deck, while keeping the game stable under Proton.

## Latest handoff (2026-07-31 — lean native-aspect build)

### Confirmed result

- The official render-manager mode-zero fix was validated on Steam Deck for
  more than one continuous hour without a crash.
- At 1280x800 it produces native `1280x800` content and output, with no
  letterbox, no stretched 720p composite, and correct scene/HUD proportions.
- A minor exit-dialog offset remains cosmetic and does not affect use.
- Grok's later `0.8.0` global HUD viewport/scissor remapping was reviewed and
  rejected because it could also modify shadow, post-processing, mip, and
  other non-HUD passes. It has been removed.

### Current implementation

- `0.9.0-lean-native-aspect` is built and directly installed.
- Installed/source DLL SHA-256:
  `9D0C0CCBC865A2BEF6D90E0F3F20445269EED62426C09F0EC1BA22E99911F9DE`.
- Installed/source INI SHA-256:
  `8C23C51981271609815570636B216C18A098CEDABD2F930A4B0F8417092E90B8`.
- The game was not running during build/deployment.
- The implementation retains only:
  - the `dinput8.dll` proxy and `DirectInput8Create` forwarding;
  - process/configuration/log handling;
  - a minimal DX11 Present hook;
  - setter-byte verification;
  - the official aspect-mode-zero request and result log.
- Obsolete camera tracing, projection upload tracing, resource expansion,
  viewport/scissor rewriting, active-rectangle scans, pattern helpers, and
  LGPL-derived runtime experiments were removed.
- Source changes reduce the project by roughly 2,300 lines. The DLL decreased
  from 286,208 bytes to 235,008 bytes.
- The DLL export table was verified to contain only `DirectInput8Create`.

### Required test

- Launch DX11 and confirm the log contains:
  - `Version 0.9.0-lean-native-aspect`;
  - `DX11 Present hook: installed`;
  - `Native aspect mode requested`;
  - `Native aspect mode applied: mode=0, content=1280x800, output=1280x800`.
- Visually confirm behavior matches the proven `0.7.6` build.
- Never build or replace the DLL while the game is running.

## Latest handoff (2026-07-30 21:05 Asia/Shanghai)

### Immediate state

- Runtime disassembly of the game's actual aspect option found the official
  render-manager modes:
  - `0`: no aspect crop;
  - `1`: 16:9, fixed aspect `1.7777778`;
  - `2`: 21:9, fixed aspect `2.3703704` (64:27).
- On the 1280x800 display, live state was verified:
  - mode 1: content `1280x720`, output `1280x800`;
  - mode 2: content `1280x540`, output `1280x800`.
- Requested mode is at render-manager `+0x7B440`; active mode is `+0x7B43C`.
  The official setter is `MonsterHunterWorld.exe+0x229C790`, and
  `+0x229EF20` applies the request and recomputes content dimensions.
- `0.7.6-native-aspect-mode-zero` is built and directly installed.
- Installed/source DLL SHA-256:
  `8BDACDAE2589C3561CE689C883762D2E8FAD63F0938D5173E8B11A369FA25CB4`.
- Windows visual testing and the live log confirm that the experiment works:
  - before the request: active mode `1`, content `1280x720`, output
    `1280x800`;
  - after the official setter applied mode `0`: active/requested mode `0`,
    content `1280x800`, output `1280x800`.
- The result has no top/bottom bars, no white/corrupt lower region, and the
  full scene/HUD is generated through the engine's native full-height path.
- On the current 16:9 Windows display, the minimap and task-board wheel look
  horizontally wide because genuine 16:10 content is being shown on a 16:9
  panel. This is expected and is the inverse of the earlier vertical-stretch
  failure. On a native 1280x800 Steam Deck panel these shapes should be round.
- Final Steam Deck validation is still required.
- The game is currently running after the successful Windows test. Do not
  build or replace the DLL until the user explicitly exits.

### `0.7.6` official mode-zero experiment

- At first Present, it obtains the real render manager through module global
  `+0x51C4480` and calls the game's official aspect setter at `+0x229C790`
  with mode `0`.
- It does not patch the shared `1.7777778` constant and does not manually
  resize textures, depth buffers, viewports, or scissors.
- The setter marks the display state dirty; the engine is expected to apply
  mode 0 through `+0x229EF20` and rebuild/recompute the content area itself.
- It logs the initial request and then waits until active mode equals
  requested mode, logging final content and output dimensions as
  `Native aspect mode applied`.
- Expected success is active/requested `0`, content `1280x800`, output
  `1280x800`, with round HUD geometry and no white/corrupt lower region.
- That expected runtime state was observed exactly:
  `Native aspect mode requested` reported mode `1 -> 0` from content
  `1280x720`, followed by `Native aspect mode applied` reporting
  mode `0`, content `1280x800`, output `1280x800`.
- Treat the official mode-zero route as the confirmed core fix. Do not return
  to shared-constant patching or manual scene-resource expansion.
- The next implementation step, after the game exits, is a clean lightweight
  release that retains only the official mode-zero request and removes the
  obsolete camera, projection-upload, resource-expansion, and high-volume
  diagnostic paths.
- The obsolete camera/singleton diagnostic functions remain in source but
  are not called.
- Never build or replace the DLL while the game is running.

## Latest handoff (2026-07-30 20:25 Asia/Shanghai)

### Immediate state

- `0.7.4` found two addresses only `0x10` apart and treated them as
  `0x1A0`/`0x17C0` camera objects. They necessarily overlap and are class
  registration metadata, not live camera instances. Its field log exhausted
  the 512-record cap during startup and contains no useful aim/FOV evidence.
- `0.7.5-camera-singleton-reference-trace` is built and directly installed.
- Installed/source DLL SHA-256:
  `6F1639D2968E53D04B2EA0100597920B5ADDC83BB05F653012BD155E4A11C369`.
- The game is not running. Launching to any rendered scene is sufficient for
  this one-shot probe; aiming is not required.

### `0.7.5` singleton-reference diagnostic

- Runtime registration identifies `sMhCamera` as size `0x1EB0`, with its
  class descriptor at module `+0x500E3F8`.
- At first Present, the diagnostic dumps the first 32 qwords of that
  descriptor and performs a read-only scan of committed private readable
  regions for exact pointers to the descriptor.
- For each reference (capped at 128), it logs four qwords before and seven
  qwords after the pointer. This should distinguish registry bookkeeping
  from an object header and expose the singleton object path.
- The invalid `0.7.4` instance scan and per-frame change trace are no longer
  called. The older `MhSky2Sun` hook is also not installed.
- No game memory is modified.
- Never build or replace the DLL while the game is running.

## Latest handoff (2026-07-30 20:15 Asia/Shanghai)

### Immediate state

- `0.7.3` was conclusively rejected: runtime parameter names identify
  `+0x1FDA390` as the `MhSky2Sun` shader-parameter builder, not a camera
  function. Its `+0x2A8/+0x2BC` changes are sky/sun interpolation.
- `0.7.4-engine-camera-instance-trace` is built and directly installed.
- Installed/source DLL SHA-256:
  `28E560AEB1409D9D3CDEEDC01EE05DC116A8CD6DFAC9F5C03C788DB8E4AB2754`.
- Scene-resource expansion, active-rectangle patching, and shared-aspect
  patching remain disabled.
- The game is not running. Launch it, enter training, aim repeatedly, then
  inspect the new `Engine camera instance` and `Engine camera field changed`
  lines.

### `0.7.4` camera-instance diagnostic

- Runtime class registration at `+0x213190` establishes `uCamera` size
  `0x1A0` and its installed class vtable at module `+0x2E4F5A0`.
- Runtime class registration at `+0x202FF0` establishes `uMhCamera` size
  `0x17C0` and its installed class vtable at module `+0x2E09AB9`.
- At first Present, the diagnostic performs one read-only
  `ReadProcessMemory` scan of committed private readable regions for exact
  instances of those two registered types.
- Found objects are sampled each Present. Changed finite float fields are
  logged by object and byte offset, capped at 512 records. No game memory is
  modified.
- If the scan finds zero objects, the active camera uses a derived class
  vtable. The next step is to enumerate the `uMh*Camera` derived class
  registrations/vtables rather than broadening the scan.
- The obsolete `MhSky2Sun` hook implementation remains in the source but is
  no longer installed.
- Never build or replace the DLL while the game is running.

## Latest handoff (2026-07-30 19:54 Asia/Shanghai)

### Immediate state

- Runtime-decrypted disassembly established that the on-disk EXE code is
  protected and cannot be meaningfully disassembled before launch.
- The shared aspect references were classified:
  - `+0x22993DA/+0x229943A/+0x229EF70/+0x229F0C4` perform dimension fitting
    and render-manager bookkeeping.
  - `+0x2304967` is final rectangle fitting.
  - `+0x242368E` is a post-processing/screen-space path.
  - `+0x1FDA863` is inside an engine render-parameter build function at
    `+0x1FDA390`, called directly from `+0x1FD57E0`.
- `+0x1FDA390` takes an object in RCX and render context in RDX. It uses
  object fields around `+0x2A8/+0x2BC`, derives width/height from the render
  context, combines them with the shared 16:9 constant, and submits results
  to the engine render parameter table. This is the best current route to
  identify FOV-derived fields.
- `0.7.3-engine-camera-parameter-trace` is built and installed.
- Installed/source DLL SHA-256:
  `16C23C9FC3F415CF2BF15BCF2FA81D4775FE5E563212F3ABE6667C4C3860B457`.
- Scene-resource expansion and aspect patching remain disabled.
- The game is not running. Launch, enter training, change normal/aimed camera
  states, then inspect `Engine camera-parameter candidate` lines.

### `0.7.3` engine hook

- Installs a detour at runtime-decrypted function `+0x1FDA390` during first
  Present.
- Verifies the first 19 bytes exactly before patching. The overwritten bytes
  are complete non-RIP-relative instructions and are copied to a trampoline.
- The hook does not modify object or render data.
- Logs only when an object's `+0x2A8`, `+0x2BC`, or packed render dimensions
  change, capped at 256 records.
- Each record contains fields `+0x2A8/+0x2AC/+0x2B0/+0x2B4/+0x2BC/+0x39C`,
  unpacked width/height, and an eight-frame call stack.
- Compare idle, aimed, and zoomed states to determine which field tracks FOV.
- Do not build or replace the DLL while the game is running.

## Latest handoff (2026-07-30 19:08 Asia/Shanghai)

### Immediate state

- `0.7.1-projection-upload-trace` correctly installed immediate-context
  Map/Unmap/UpdateSubresource hooks, but training-area camera movement and
  aiming produced zero conservative projection candidates.
- `0.7.2-deferred-projection-trace` is built and installed.
- Installed/source DLL SHA-256:
  `DAEF5FC456DE6C49AB18816E030267E9F08983ECE80A48A0C173369FD0D56D6F`.
- Scene-resource expansion and aspect patching remain disabled.
- The game is not running. Launch, enter the training area, rotate and aim,
  then inspect `DX11 deferred context` and `DX11 projection candidate` lines.

### Deferred-context implementation

- Hooks `ID3D11Device::CreateDeferredContext` at vtable index 27 during the
  early dummy-device setup, alongside the early CreateTexture2D hook.
- Each returned deferred context has Map/Unmap/UpdateSubresource hooked.
- Upload originals are stored per context-vtable in a bounded registry, so
  shared versus distinct immediate/deferred implementations do not overwrite
  one global original pointer or recurse.
- The immediate context also uses this registry when its upload hooks are
  installed at first Present.
- Deferred-context creation is logged with a sequence number and hook status.
- No resource or uploaded content is modified.
- If no deferred contexts are created or candidates remain zero, stop adding
  broad D3D upload hooks. Move to static/runtime analysis of the engine-side
  projection generator or support combined view-projection matrix detection.
- Do not build or replace the DLL while the game is running.

## Latest handoff (2026-07-30 18:34 Asia/Shanghai)

### Immediate state

- `0.7.0-projection-matrix-trace` loaded correctly and its real immediate
  context Map/Unmap hooks installed, but a full in-game camera movement and
  aiming test produced zero conservative 16:9 projection candidates.
- This rules out ordinary immediate-context Map/Unmap uploads for the target
  matrix, assuming the matrix layout filter is correct.
- `0.7.1-projection-upload-trace` is built and installed. It retains the
  safe stock render pipeline and adds read-only `UpdateSubresource` tracing
  for D3D11 constant buffers.
- Installed/source DLL SHA-256:
  `6600DD3E2106E6C871CA0445DB496F3BC4ED9FC24AD5EEAC7A42DC3B0214A7AD`.
- Scene-resource expansion and aspect patching remain disabled.
- The game is not running. Launch, enter the task-board scene, rotate the
  camera, and aim once before inspecting the log.

### `0.7.1` diagnostics

- Hooks real immediate-context vtable entry 48 (`UpdateSubresource`) at first
  Present in addition to Map/Unmap.
- For constant-buffer destinations of 64..65536 bytes, scans the uploaded
  data using the same conservative 16:9 perspective-matrix predicate.
- A hit logs `path=UpdateSubresource`, matrix values, buffer size/offset, and
  an eight-frame call stack. Map/Unmap hits now also log the full stack.
- No uploaded content is modified.
- If this still produces zero candidates, next inspect deferred contexts
  and/or identify the engine-side projection generator statically rather
  than broadening into an unbounded runtime scan.
- Do not build or replace the DLL while the game is running.

## Latest handoff (2026-07-30 18:18 Asia/Shanghai)

### Immediate state

- The user confirmed that `0.6.9-safe-stock-pipeline` restores round radar
  and task-board geometry. The live log confirmed the real swap chain is
  1280x800.
- `0.7.0-projection-matrix-trace` is now built and installed.
- Installed/source DLL SHA-256:
  `14B34F1BF9C975A3C92D4A4E3185D7A4E4938657E921972AB9F178D70B55DC9B`.
- Both scene-resource expansion and aspect patching remain disabled. This
  build must preserve the safe stock visual result.
- The game is not running. The user should launch it and enter the task-board
  scene, then report that they are in game so the log can be inspected.

### Projection trace

- The real immediate context's `Map` and `Unmap` entries are hooked at first
  Present, alongside the existing viewport/scissor hooks.
- Write-discard/no-overwrite mappings of D3D11 constant buffers are inspected
  immediately before Unmap. No buffer content is modified.
- The scanner looks at 16-byte-aligned 4x4 float blocks for perspective
  matrices whose absolute `m11/m00` ratio is approximately 16:9, whose
  bottom-right element is zero, and whose row- or column-major perspective
  term is approximately one.
- Unique candidates are logged as `DX11 projection candidate`, including
  buffer pointer, byte width, offset, selected matrix values, ratio, and the
  immediate Unmap caller.
- If no candidates appear, the engine may upload through
  `UpdateSubresource`, use deferred contexts, or use a matrix layout that
  does not match this conservative filter. Extend diagnosis rather than
  weakening it into an unbounded high-volume scan.
- Do not build or replace the DLL while the game is running.

## Latest handoff (2026-07-30 18:08 Asia/Shanghai)

### Immediate state

- `0.6.8-full-height-no-aspect-patch` was visually rejected. The scene and
  HUD remained vertically stretched, the radar was clipped, and the title
  screen showed a large white/uninitialized lower region.
- This proves that selectively expanding only the exact long-lived scene
  color/depth resources and their viewport is not a valid standalone fix.
  Other post-processing/compositor resources and 720-high sampling/layout
  state remain coupled to that pipeline.
- Do not add a camera-aspect experiment on top of this incomplete resource
  expansion. The resource experiment must remain disabled until the full
  paired render/post-processing chain is identified.
- `0.6.9-safe-stock-pipeline` is built and installed. Both
  `ExperimentalExpandSceneResources` and
  `ExperimentalPatchAspectConstant` are false.
- Installed/source DLL SHA-256:
  `EE100FDE9387E55D865940C11771D5E70FE93724F9A04615A010C8CCEE8769EB`.
- The game is not running. The user should launch and verify that stock
  geometry/UI and the title screen are restored; 40-pixel top/bottom bars
  are expected in this safety baseline.

### Next investigation

- Keep the safe stock pipeline while tracing the exact post-processing and
  compositor resources that consume the long-lived format-26 scene color
  texture.
- Identify the full resource/view/sampling chain before changing any
  resource dimensions again.
- Do not build or replace the DLL while the game is running.

## Latest handoff (2026-07-30 18:02 Asia/Shanghai)

### Immediate state

- The user reported that `0.6.7-full-height-final-rect-only` still rendered
  the game and UI much too tall. The white/corrupted lower region also
  remained visible.
- The `0.6.7` log confirmed that both exact long-lived scene resources and
  the matching viewport were expanded to 1280x800. It also confirmed that
  the selective aspect patch was active. This rules out a missed hook and
  supports the double-expansion diagnosis.
- `0.6.8-full-height-no-aspect-patch` is now built and installed. It keeps
  `ExperimentalExpandSceneResources=true` and sets
  `ExperimentalPatchAspectConstant=false`.
- Installed/source DLL SHA-256:
  `04A0EB56061FB4044EA1F1B978DCFA5CAE17BAB36B5A52BF9066790D11B934AB`.
- The game is not running. The next action is for the user to launch and
  visually test `0.6.8`.

### What `0.6.8` tests

- Exact long-lived format-26 scene color and format-39 scene depth resources
  still expand from 1280x720 to 1280x800.
- Their matching full-frame viewport still expands to 1280x800.
- No shared 16:9 constant is changed, including the final rectangle-fitting
  reference at `+0x2304967`.
- This isolates whether the real 800-high source can flow through the stock
  compositor without the second 720-to-800 stretch.

### What to check next

- Confirm from the startup log that version `0.6.8` loaded, scene color,
  depth, and viewport expansion all hit, and no “Selective aspect patch
  applied” line appears.
- Visually check whether the task-board crest/minimap and character geometry
  are round/proportional, whether the white lower region is gone, and whether
  top/bottom bars return.
- Do not build or replace the DLL while the game is running.

## Latest handoff (2026-07-30 17:57 Asia/Shanghai)

### Immediate state

- Current source/deployed experiment:
  `0.6.7-full-height-final-rect-only`.
- Built and installed DLL SHA-256 (source and destination match):
  `CCE363F25DF7813078F59C88020FB21738E815A1C954A7503D5ADFEFD05FA364`.
- The game is currently running as PID 24344, started at approximately
  17:55 Asia/Shanghai.
- The user has not yet reported the visual result for `0.6.7`.
- While the game is running, read-only log inspection is allowed, but do not
  compile or replace the DLL. Wait for an explicit user report and explicit
  exit before the next build/deployment.
- Installed config has:
  `ExperimentalExpandSceneResources=true`.

### Git and uncommitted work

- Baseline commits are still:
  - `53c34d2 Initial MHW 16:10 DX11 diagnostics`
  - `e832d64 Add experimental 16:10 aspect constant patch`
- Important uncommitted files:
  - `AGENTS.md`
  - `config/mhw_16x10.ini`
  - `src/common.hpp`
  - `src/config.cpp`
  - `src/config.hpp`
  - `src/d3d11_diagnostics.cpp`
- Do not discard these changes. They include selective aspect experiments,
  early resource identity tracing, exact main-scene resource selection, and
  the safe gating of the old active-rectangle memory scan.
- Git commands may require:
  `git -c safe.directory='E:/repository/MHW16x10Fix'`.

### Key visual diagnosis

- The known no-letterbox baseline globally changes the shared 16:9 constant
  at `MonsterHunterWorld.exe+0x2F74AD4` to 1.6.
- The game normally renders the 3D scene and HUD into 1280x720 resources and
  then stretches the combined result to 1280x800.
- Circular references provide an unambiguous visual test: the task-board
  circular crest and the minimap/radar become vertically elongated, proving
  that the whole final composite is stretched by `800/720 = 1.111...`.
- Merely patching the global aspect constant removes the top/bottom bars but
  stretches both 3D and UI.

### Exhausted shared-constant experiments

The shared constant has eight SSE references:

- `+0x1FDA863`
- `+0x22993DA`
- `+0x229943A`
- `+0x229A903`
- `+0x229EF70`
- `+0x229F0C4`
- `+0x2304967`
- `+0x242368E`

Results:

- `0.5.8-camera-aspect-test`: leaving `+0x1FDA863` on 1.6 did not
  correct the 3D stretch.
- `0.5.9-screen-space-aspect-test`: leaving `+0x242368E` on 1.6
  introduced left/right black bars and did not correct the 3D stretch.
  This is a final screen-fitting path and should normally stay 16:9.
- `0.5.10-layout-path-a-test`: `+0x229943A` on 1.6 made no visible
  improvement.
- `0.5.11-layout-path-b-test`: `+0x229A903` on 1.6 made no visible
  improvement.
- Conclusion: none of these individual shared-constant references is an
  independent 3D-camera correction. Do not repeat these single-variable
  tests.

### Resource tracing and exact main-scene resources

- A new config option and implementation were added:
  `ExperimentalExpandSceneResources`.
- Resource identity tracing records each candidate texture object, its
  descriptor, and its creation stack, then correlates it with the RTV/DSV
  actually bound when the 1280x720 main viewport is set.
- Correct hook timing is essential:
  - Hook `ID3D11Device::CreateTexture2D` immediately during diagnostic dummy
    device setup. The real device shares this device vtable entry, and the
    long-lived resources are created before first Present.
  - Hook the real immediate context's `RSSetViewports` and
    `RSSetScissorRects` only at the first real Present. The dummy context
    does not share the real context vtable.
  - Do not install `CreateTexture2D` again at Present; doing so can replace
    the stored original pointer with the hook and recurse.
- Earlier dynamic resource candidates were:
  - color format 61 at `+0x22ECD2C`
  - depth format 39 at `+0x22ECD7D`
  - color format 26 at `+0x22ECDE5`
- Expanding that `+0x22ECD*` group in versions `0.6.0`/`0.6.1`
  successfully changed resources and viewport to 800, but made no visible
  improvement. It is a later dynamic resource group, not the long-lived
  visible main-scene buffer. Do not return to it.
- The real long-lived visible main-scene color resource was identified:
  - size/format: 1280x720, DXGI format 26
  - bind flags: `0xA8`
  - creation stack core:
    `+0x24F1F23 -> +0x24F19E5 -> +0x1AED933`
- Its paired long-lived depth resource was identified by the adjacent
  initialization chain:
  - size/format: 1280x720, DXGI format 39
  - bind flags: `0x48`
  - creation stack core:
    `+0x23BCBDC -> +0x23BC510 -> +0x1AED61A -> +0x22E4B14`
- The relevant render binding stack remains:
  `+0x259B359 -> +0x2599065 -> +0x2298716 -> +0x229CEC1
  -> +0x229AC31`.

### Resource experiments

- `0.6.6-main-scene-resource-test` selectively expanded only the exact
  long-lived format-26 color resource and paired format-39 depth resource
  from 1280x720 to 1280x800, then changed the matching viewport to
  1280x800.
- The patch definitely hit both exact resources and the viewport.
- Visual result: the scene became even taller and UI/post-processing showed
  corruption, including a white lower region.
- Diagnosis: this caused double vertical expansion. The scene was rendered
  into an 800-high viewport, and the existing global 1.6 aspect/compositor
  patch stretched the final composite again. Some UI/post-processing
  resources still remained 720 high, causing the lower-region anomaly.

### Current `0.6.7` experiment

- Keep exact long-lived main-scene color/depth expansion enabled.
- Keep matching main-scene viewport expansion enabled.
- Redirect seven shared-constant consumers back to untouched 16:9:
  - `+0x1FDA863`
  - `+0x22993DA`
  - `+0x229943A`
  - `+0x229A903`
  - `+0x229EF70`
  - `+0x229F0C4`
  - `+0x242368E`
- Leave only final rectangle-fitting reference `+0x2304967` using the
  patched 1.6 constant.
- Rationale: the earlier `0.5.6` final-rectangle-only test restored bars
  when the source was still 720 high. Combining it with a real 800-high
  source may allow full-height output without the second stretch.
- This version is currently running and awaiting the user's result.
- When the user reports, specifically check:
  - whether top/bottom or left/right bars exist;
  - whether the task-board circular crest and minimap are round;
  - whether character geometry is proportioned correctly;
  - whether the white lower-region/UI corruption from `0.6.6` is gone.

### Likely next decisions after `0.6.7`

- If `0.6.7` gives correct geometry/full height:
  - preserve the resource/final-rectangle combination;
  - trace and separately correct the HUD/scissor 720-safe-area layout.
- If bars return:
  - inspect the live log first to confirm both exact resources and viewport
    still expanded;
  - the remaining issue is the compositor/source rectangle relationship,
    not resource identification.
- If double stretch remains:
  - the final rectangle path is still scaling the 800 source; test removing
    the shared aspect constant patch entirely while keeping exact resource
    expansion, or apply an inverse source-rectangle correction rather than
    expanding more resources.
- If UI corruption remains:
  - do not blindly expand every 720p texture;
  - trace the precise post-processing/UI resource bound immediately before
    the 1280x800 swap-chain pass.

### Safety and deployment

- Never compile or replace the DLL while MonsterHunterWorld.exe is running.
- The user requested direct replacement without backups for these rapid
  experiments.
- Build command:
  `cmd.exe /d /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build build-ninja --config Release'`
- Build output:
  `E:\repository\MHW16x10Fix\build-ninja\dinput8.dll`
- Game destination:
  `E:\SteamLibrary\steamapps\common\Monster Hunter World\dinput8.dll`
- Always compare source/destination SHA-256 after copying.
- The old full writable-memory active-rectangle scan is unsafe during normal
  play and is gated behind `ExperimentalPatchActiveRect=false`. Do not
  enable it for ordinary tests.

## Latest handoff (2026-07-30 16:40 Asia/Shanghai)

### Git and working tree

- Repository was initialized and the original diagnostic baseline was
  committed as `53c34d2 Initial MHW 16:10 DX11 diagnostics`.
- The first working no-letterbox milestone was committed as
  `e832d64 Add experimental 16:10 aspect constant patch`.
- There are currently uncommitted changes after `e832d64` in:
  - `src/common.hpp`
  - `src/d3d11_diagnostics.cpp`
- Do not discard these changes. They contain the selective scene/HUD
  experiments and the fix for an unsafe diagnostic scan.
- Git commands may require:
  `git -c safe.directory='E:/repository/MHW16x10Fix'`.

### Confirmed breakthrough and limitation

- Runtime float constant `MonsterHunterWorld.exe+0x2F74AD4` is the relevant
  shared 16:9 value (`0x3FE38E39`, 1.7777778).
- Changing that constant to 1.6 removes the top and bottom black bars on
  Steam Deck at 1280x800.
- This was verified on Steam Deck with version `0.5.5-aspect-constant`.
- However, the minimap, text, menus, and other HUD elements are vertically
  stretched by exactly `800 / 720 = 1.111...`.
- The game still creates and renders through many real 1280x720 scene
  resources. The constant patch expands the final 1280x720 composite
  (3D scene plus HUD) to 1280x800; it does not change the underlying render
  resources to 800 pixels high.
- Therefore, the final compositor binds the scene and HUD together. Merely
  changing this shared constant can only stretch both. A complete solution
  must eventually keep the full-height composition, correct the 3D camera
  projection independently, and then correct the HUD independently.

### Runtime 16:9 constant references

The relevant constant at `+0x2F74AD4` has these eight runtime SSE references:

- `+0x1FDA863`
- `+0x22993DA`
- `+0x229943A`
- `+0x229A903`
- `+0x229EF70`
- `+0x229F0C4`
- `+0x2304967`
- `+0x242368E`

Observed grouping:

- `+0x22993DA`, `+0x229EF70`, and `+0x229F0C4` use render-manager field
  `+0x7B43C`.
- `+0x229943A` and `+0x229A903` use field `+0x7B440`.
- `+0x2304967` operates rectangle fields at `+0x5C/+0x60/+0x64/+0x68` and
  appears to be a final rectangle-fitting path.
- `+0x1FDA863` is currently the strongest camera/render-target aspect
  candidate. It reads packed dimensions and performs aspect math.
- `+0x242368E` appears more likely to be post-processing or screen-space
  effect math and should be left unchanged during the next single-variable
  test.

### Tested versions and results

1. `0.5.5-aspect-constant`
   - Patched the shared `+0x2F74AD4` value globally from 16:9 to 1.6.
   - Steam Deck: no black bars.
   - 3D scene appeared acceptable.
   - HUD, minimap, and text were vertically stretched.
   - This is the current known-good no-letterbox baseline.

2. `0.5.6-selective-composite`
   - Left only `+0x2304967` on 1.6 and redirected the other seven references
     to the untouched 16:9 constant at `+0x229990E`.
   - Windows: HUD proportions were correct and the game was stable after the
     scan fix.
   - Steam Deck: the 40-pixel top and bottom black bars returned.
   - Conclusion: `+0x2304967` alone is insufficient to remove letterboxing.

3. `0.5.7-split-scene-hud` (current source/deployed Windows experiment)
   - Leaves `+0x22993DA`, `+0x229EF70`, `+0x229F0C4`, and `+0x2304967`
     on 1.6.
   - Redirects `+0x1FDA863`, `+0x229943A`, `+0x229A903`, and
     `+0x242368E` to the untouched 16:9 constant at `+0x229990E`.
   - Windows result: both the 3D scene and HUD are vertically stretched.
   - This strongly suggests that one of the redirected references is needed
     to correct the 3D camera projection while the final image is expanded.

### Next exact experiment

Keep the current no-letterbox/final-expansion paths at 1.6 and change only
the strongest camera candidate:

- Remove `+0x1FDA863` from the `ui_xrefs` redirection array so it once again
  reads the patched 1.6 constant.
- Continue redirecting these three references to 16:9:
  - `+0x229943A`
  - `+0x229A903`
  - `+0x242368E`
- Suggested version name: `0.5.8-camera-aspect-test`.
- Test whether the 3D scene geometry becomes correctly proportioned while
  the HUD remains vertically stretched.
- Do not change `+0x242368E` in the same test. If `+0x1FDA863` does not fix
  the 3D ratio, test `+0x242368E` separately afterward.
- The user explicitly wants to proceed in this order:
  1. preserve the current full-screen/stretch behavior;
  2. fix the 3D scene ratio;
  3. handle HUD scaling and vertical centering last.

### Crash investigation resolved

- The first `0.5.6` Windows run crashed with `0xC0000005` at
  `DINPUT8.dll+0x157BD`.
- Windows Event Viewer identified the faulting module as this proxy DLL, not
  the game executable. It was not DRM or an executable integrity check.
- Disassembly mapped the fault to `scan_active_render_rectangle`, which was
  reading a writable game allocation while the engine decommitted it.
- Current source fixes this by calling `start_active_rect_trace()` only when
  `ExperimentalPatchActiveRect=true`.
- The normal configuration has `ExperimentalPatchActiveRect=false`.
- Do not re-enable the full writable-memory scan for ordinary tests.

### Latest known build/deployment hashes

- Stable post-scan-fix `0.5.6-selective-composite`:
  `2BD442781B81578E03B93F7933993D6E04E5A8380E0357EB2AB2DE4D2FDF28D5`
- Current `0.5.7-split-scene-hud`:
  `A7C05424A780FF70783290ABD64F87568C84BBFF9DF5D463590661A79D9952E6`

### Build and deployment procedure

Build:

`cmd.exe /d /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build build-ninja --config Release'`

Output:

`E:\repository\MHW16x10Fix\build-ninja\dinput8.dll`

Windows game directory:

`E:\SteamLibrary\steamapps\common\Monster Hunter World`

- The user explicitly requested direct replacement without backups.
- Nevertheless, always confirm `MonsterHunterWorld.exe` is not running
  before building or copying.
- Copy the DLL directly and compare source/destination SHA-256 afterward.
- The game state at the end of this handoff was not explicitly confirmed;
  ask/wait for an explicit exit before compiling or deploying the next DLL.

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
