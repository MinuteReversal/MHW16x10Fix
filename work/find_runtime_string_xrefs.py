import ctypes
import pathlib
import sys
from ctypes import wintypes

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP


EXE = pathlib.Path(
    r"E:\SteamLibrary\steamapps\common\Monster Hunter World"
    r"\MonsterHunterWorld.exe"
)
TARGETS = {
    0x350D3F0: "aspect enum 4:3",
    0x350D3F4: "aspect enum 16:9",
    0x350D3FC: "aspect enum 16:10",
    0x350D404: "aspect enum 21:9",
    0x350D40C: "aspect enum 32:9",
    0x350D380: "aspect enum table",
    0x3E89D28: "GraphicsOption Aspect Ratio key pointer",
    0x2F74588: "GraphicsOption Aspect Ratio",
    0x2F74598: "GraphicsOption Ultrawide UI",
    0x500E3F8: "sMhCamera descriptor",
    0x3498EE0: "uMhFreeCamera",
    0x349A3C8: "uMhMotionCamera",
    0x3498810: "uMhEventCamera",
    0x3499628: "uMhMapCamera",
    0x349A960: "uMhRailCamera",
    0x349A808: "uMhDistRailCamera",
    0x349AC28: "uMhSimpleCamera",
    0x3497098: "uMhCamera",
    0x3497D08: "CBViewProjection",
    0x349C258: "mBaseFov",
    0x3346BC8: "mFov",
    0x33EED58: "mViewParam.Fov",
    0x33F8278: "sMhCamera",
    0x350C880: "fViewProj",
    0x351C078: "viewProjMatrix",
    0x351FBB0: "uCamera",
    0x3521948: "fCameraPos",
    0x3521990: "fCameraNearClip",
    0x35219A0: "fCameraFarClip",
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
        raise ctypes.WinError(ctypes.get_last_error())
    return output.raw[: count.value]


pe = pefile.PE(str(EXE), fast_load=False)
entries = sorted(
    pe.DIRECTORY_ENTRY_EXCEPTION,
    key=lambda entry: entry.struct.BeginAddress,
)


def containing_function(rva):
    low, high = 0, len(entries)
    while low < high:
        middle = (low + high) // 2
        entry = entries[middle]
        if rva < entry.struct.BeginAddress:
            high = middle
        elif rva >= entry.struct.EndAddress:
            low = middle + 1
        else:
            root = entry
            while getattr(root.unwindinfo, "_chained_entry", None):
                root = root.unwindinfo._chained_entry
            return root.struct.BeginAddress, entry.struct.EndAddress
    return None


text = next(
    section
    for section in pe.sections
    if section.Name.rstrip(b"\0") == b".text"
)
text_size = max(text.Misc_VirtualSize, text.SizeOfRawData)
code = read(base + text.VirtualAddress, text_size)
target_addresses = {base + rva: (rva, name) for rva, name in TARGETS.items()}
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
hits = []
for instruction in md.disasm(code, base + text.VirtualAddress):
    for operand in instruction.operands:
        if operand.type != X86_OP_MEM or operand.mem.base != X86_REG_RIP:
            continue
        target = instruction.address + instruction.size + operand.mem.disp
        if target not in target_addresses:
            continue
        target_rva, name = target_addresses[target]
        source_rva = instruction.address - base
        hits.append((name, target_rva, source_rva, instruction, containing_function(source_rva)))

for name, target_rva, source_rva, instruction, function in hits:
    print(
        f"{name} +0x{target_rva:X} <- +0x{source_rva:X} "
        f"{instruction.mnemonic} {instruction.op_str}; function={function}"
    )
print(f"hit_count={len(hits)}")
kernel32.CloseHandle(handle)
