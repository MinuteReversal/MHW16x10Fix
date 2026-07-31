import ctypes
import pathlib
import struct
import sys
from ctypes import wintypes

import pefile


EXE = pathlib.Path(
    r"E:\SteamLibrary\steamapps\common\Monster Hunter World"
    r"\MonsterHunterWorld.exe"
)
TARGETS = {
    0x3346BC8: "mFov",
    0x3346BD0: "mFov .mValue",
    0x33EED58: "mViewParam.Fov",
    0x349C258: "mBaseFov",
    0x349C268: "mSkyParam.mBaseFov",
    0x353C3F0: "mFOV",
}
pid = int(sys.argv[1])
base = int(sys.argv[2], 0)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.OpenProcess.restype = wintypes.HANDLE
kernel32.ReadProcessMemory.argtypes = [
    wintypes.HANDLE,
    wintypes.LPCVOID,
    wintypes.LPVOID,
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_size_t),
]
handle = kernel32.OpenProcess(0x10 | 0x400, False, pid)


def read(address, size):
    output = ctypes.create_string_buffer(size)
    count = ctypes.c_size_t()
    if not kernel32.ReadProcessMemory(
        handle, ctypes.c_void_p(address), output, size, ctypes.byref(count)
    ):
        return b""
    return output.raw[: count.value]


pe = pefile.PE(str(EXE), fast_load=False)
for target_rva, name in TARGETS.items():
    needles = [
        ("absolute64", struct.pack("<Q", base + target_rva)),
        ("rva32", struct.pack("<I", target_rva)),
    ]
    print(f"\n=== {name} +0x{target_rva:X} ===")
    hit_count = 0
    for section in pe.sections:
        size = max(section.Misc_VirtualSize, section.SizeOfRawData)
        if size == 0:
            continue
        blob = read(base + section.VirtualAddress, size)
        for needle_kind, needle in needles:
            cursor = 0
            while blob:
                index = blob.find(needle, cursor)
                if index < 0:
                    break
                hit_rva = section.VirtualAddress + index
                nearby = read(base + hit_rva - 0x30, 0x90)
                dwords = struct.unpack(
                    "<" + "I" * (len(nearby) // 4), nearby
                )
                print(
                    f"hit +0x{hit_rva:X} kind={needle_kind} section="
                    f"{section.Name.rstrip(bytes([0])).decode(errors='replace')}"
                )
                print(
                    " dwords="
                    + " ".join(f"0x{value:X}" for value in dwords)
                )
                hit_count += 1
                cursor = index + 1
    print(f"hit_count={hit_count}")
kernel32.CloseHandle(handle)
