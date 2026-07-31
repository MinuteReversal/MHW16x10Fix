import ctypes
import pathlib
import sys
from ctypes import wintypes

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86_const import X86_OP_MEM


EXE = pathlib.Path(
    r"E:\SteamLibrary\steamapps\common\Monster Hunter World"
    r"\MonsterHunterWorld.exe"
)
FIELDS = {0x7B43C, 0x7B440}
pid = int(sys.argv[1])
base = int(sys.argv[2], 0)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.OpenProcess.restype = wintypes.HANDLE
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
text = next(section for section in pe.sections if section.Name.startswith(b".text"))
blob = read(base + text.VirtualAddress, text.Misc_VirtualSize)
decoder = Cs(CS_ARCH_X86, CS_MODE_64)
decoder.detail = True
decoder.skipdata = True
hits = set()
for field in FIELDS:
    needle = field.to_bytes(4, "little")
    cursor = 0
    while True:
        index = blob.find(needle, cursor)
        if index < 0:
            break
        hits.add(base + text.VirtualAddress + index)
        cursor = index + 1
for displacement_address in sorted(hits):
    start = displacement_address - 0x48
    address = None
    candidates = list(decoder.disasm(read(start, 0xB0), start))
    for candidate in candidates:
        if candidate.address <= displacement_address < candidate.address + candidate.size:
            address = candidate.address
            break
    if address is None:
        continue
    start = address - 0x40
    nearby_instructions = list(decoder.disasm(read(start, 0xA0), start))
    print(f"\n=== +0x{address-base:X} ===")
    for nearby in nearby_instructions:
        if nearby.address > address + 0x40:
            break
        marker = ">>" if nearby.address == address else "  "
        print(
            f"{marker} +0x{nearby.address-base:08X} "
            f"{nearby.mnemonic:8} {nearby.op_str}"
        )
kernel32.CloseHandle(handle)
