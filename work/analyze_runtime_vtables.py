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
TARGET_RVAS = [
    0x1FDA6B8,
    0x229A8C0,
    0x229EF20,
    0x229F090,
    0x230493C,
    0x242359E,
]
PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400

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
handle = kernel32.OpenProcess(
    PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid
)
if not handle:
    raise ctypes.WinError(ctypes.get_last_error())


def read(address: int, size: int) -> bytes:
    output = ctypes.create_string_buffer(size)
    count = ctypes.c_size_t()
    if not kernel32.ReadProcessMemory(
        handle, ctypes.c_void_p(address), output, size, ctypes.byref(count)
    ):
        return b""
    return output.raw[: count.value]


def c_string(address: int, limit: int = 256) -> str:
    raw = read(address, limit)
    raw = raw.split(b"\0", 1)[0]
    return raw.decode("ascii", errors="replace")


pe = pefile.PE(str(EXE), fast_load=False)
sections = []
for section in pe.sections:
    size = max(section.Misc_VirtualSize, section.SizeOfRawData)
    if size == 0:
        continue
    blob = read(base + section.VirtualAddress, size)
    if blob:
        sections.append((section, blob))

for target_rva in TARGET_RVAS:
    needle = struct.pack("<Q", base + target_rva)
    print(f"\n=== function +0x{target_rva:X} ===")
    hits = []
    for section, blob in sections:
        cursor = 0
        while True:
            index = blob.find(needle, cursor)
            if index < 0:
                break
            hit_rva = section.VirtualAddress + index
            hits.append(hit_rva)
            cursor = index + 1
    print("pointer_hits=" + ", ".join(f"+0x{x:X}" for x in hits[:64]))
    for hit_rva in hits[:16]:
        print(f"  hit +0x{hit_rva:X}")
        nearby = read(base + hit_rva - 0x28, 0x70)
        values = struct.unpack("<" + "Q" * (len(nearby) // 8), nearby)
        print(
            "    qwords="
            + " ".join(
                f"{value-base:+#x}" if base <= value < base + 0x40000000
                else f"0x{value:X}"
                for value in values
            )
        )
        locator_ptr_raw = read(base + hit_rva - 8, 8)
        if len(locator_ptr_raw) != 8:
            continue
        locator = struct.unpack("<Q", locator_ptr_raw)[0]
        locator_raw = read(locator, 24)
        if len(locator_raw) != 24:
            continue
        signature, offset, cd_offset, type_rva, hierarchy_rva, self_rva = (
            struct.unpack("<IIIIII", locator_raw)
        )
        if signature == 1 and self_rva == locator - base:
            name = c_string(base + type_rva + 16)
            print(
                f"    RTTI offset=0x{offset:X} type=+0x{type_rva:X} "
                f"name={name}"
            )

kernel32.CloseHandle(handle)
