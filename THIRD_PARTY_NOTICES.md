# Third-party notices

## Lazy Aspect Fix

The runtime render-structure search and replacement algorithm in
`src/runtime_patch.cpp` is adapted from:

- **Lazy Aspect Fix**, by James Warren / Mace ya face
- Source: <https://gitlab.com/Mace_ya_face/lazy-aspect-fix>
- License: GNU Lesser General Public License version 3

The corresponding license text is included at
`LICENSES/LGPL-3.0.txt`.

The original project is an external Windows patcher and does not support
DirectX 12. MHW16x10Fix reimplements the relevant algorithm inside the game
process for use through a `dinput8.dll` proxy. The adapted implementation is
materially changed to add exact-match validation, transactional rollback,
logging, delayed initialization, and SteamOS/Proton-oriented loading.

