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

Status: **analyzed, not yet patch-supported**.

### Static aspect-ratio candidates

The little-endian IEEE-754 value for `16.0f / 9.0f`
(`39 8E E3 3F`) occurs only three times:

| File offset | RVA | Initial classification |
| --- | --- | --- |
| `0x2F730D4` | `0x2F74AD4` | Generic constants table |
| `0x350B874` | `0x350D274` | Render metadata near `mViewport`, `mVirtualRect`, and `sRender`; dynamic verification required |
| `0x3C09E40` | `0x3C0B840` | Numeric lookup table |

None of these offsets is approved for patching. The candidate at
`0x350B874` may describe reflected metadata rather than a live projection or
letterbox value. A runtime data breakpoint or RenderDoc capture is required
before deriving a signature and expected original instruction bytes.

### Local graphics state during analysis

- `graphics_option.ini`: `Resolution=1280x800`
- `graphics_option.ini`: `ScreenMode=Borderless`
- `graphics_option.ini`: `DirectX12Enable=On`
- `config.ini`: `EnableDX12=OFF`
- The in-game aspect-ratio selector exposes only 16:9 and 21:9.
- `Aspect Ratio=Off` maps to 16:9.
- `Aspect Ratio=On` maps to 21:9; it is a Boolean mode flag, not a numeric
  aspect-ratio value, so `1.6` cannot be supplied through the INI.

The captured 21:9 view has large top and bottom bars. The next diagnostic
capture should use the 16:9 option at 1280x800. The initial fix should keep
the 16:9 HUD safe area, expand the gameplay render region from 1280x720 to
1280x800, and correct projection to 1.6. Do not assume which of the two DX12
flags wins.

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
