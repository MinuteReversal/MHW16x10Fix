# Analyzed game builds

## Steam x64 build dated 2024-08-30

Observed locally on 2026-07-30:

| Property | Value |
| --- | --- |
| Executable | `MonsterHunterWorld.exe` |
| Size | `84,225,952` bytes |
| SHA-256 | `C2EBBBD2C49F216D484E31A5219BED419EB1E5E7D206D02CBA040A3AB79D90EA` |
| PE timestamp | `2024-08-30 11:29:58` |
| Architecture | x86-64 / PE32+ |
| Image size | `0x55D3000` |

Status: **supported by the DX11 native-aspect mode-zero fix**.

### Static aspect-ratio candidates

The little-endian IEEE-754 value for `16.0f / 9.0f`
(`39 8E E3 3F`) occurs only three times:

| File offset | RVA | Initial classification |
| --- | --- | --- |
| `0x2F730D4` | `0x2F74AD4` | Generic constants table |
| `0x350B874` | `0x350D274` | Render metadata near `mViewport`, `mVirtualRect`, and `sRender`; dynamic verification required |
| `0x3C09E40` | `0x3C0B840` | Numeric lookup table |

The final fix does not patch these constants. Runtime disassembly identified
the official render-manager aspect setter at RVA `0x229C790` and the render
manager global at RVA `0x51C4480`.

The verified render modes are:

- mode `0`: no aspect crop;
- mode `1`: 16:9;
- mode `2`: 21:9 (64:27).

At 1280x800, switching from mode 1 to mode 0 changes the engine content area
from 1280x720 to 1280x800 while the output remains 1280x800.

### Local graphics state during analysis

- `graphics_option.ini`: `Resolution=1280x800`
- `graphics_option.ini`: `ScreenMode=Borderless`
- `graphics_option.ini`: `DirectX12Enable=On`
- `config.ini`: `EnableDX12=OFF`
- The in-game aspect-ratio selector exposes only 16:9 and 21:9.
- `Aspect Ratio=Off` maps to 16:9.
- `Aspect Ratio=On` maps to 21:9; it is a Boolean mode flag, not a numeric
  aspect-ratio value, so `1.6` cannot be supplied through the INI.

DX12 remains unsupported. The fix reads this setting only to fail closed
before installing its DX11 Present hook.

### Steam Deck 1280x800 confirmation

A Steam Deck screenshot from game version `15.23.00` was measured at exactly
`1280x800`. Across sample columns at x=320, x=640, and x=960:

- first visible content row: `y=40`
- last visible content row: `y=759`
- active content height: `720` pixels
- top letterbox: `40` pixels
- bottom letterbox: `40` pixels

This confirms that the game renders a centered `1280x720` 16:9 region inside
the native `1280x800` output. The primary acceptance criterion is therefore a
full-height active region from `y=0` through `y=799`, without vertically
stretching the existing 3D image.

That acceptance criterion was met through official render mode 0. A Steam Deck
session exceeding one hour completed without a crash and with correct scene
and HUD proportions. A minor exit-dialog positioning issue remains cosmetic.
