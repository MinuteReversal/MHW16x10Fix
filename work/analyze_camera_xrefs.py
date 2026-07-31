import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent / "pydeps"))

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM


EXE = pathlib.Path(
    r"E:\SteamLibrary\steamapps\common\Monster Hunter World"
    r"\MonsterHunterWorld.exe"
)
TARGETS = [
    0x1FDA863,
    0x22993DA,
    0x229943A,
    0x229A903,
    0x229EF70,
    0x229F0C4,
    0x2304967,
    0x242368E,
]

pe = pefile.PE(str(EXE), fast_load=False)
image_base = pe.OPTIONAL_HEADER.ImageBase
data = EXE.read_bytes()
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True


def rva_bytes(rva: int, size: int) -> bytes:
    offset = pe.get_offset_from_rva(rva)
    return data[offset : offset + size]


runtime_functions = []
if hasattr(pe, "DIRECTORY_ENTRY_EXCEPTION"):
    for entry in pe.DIRECTORY_ENTRY_EXCEPTION:
        begin = entry.struct.BeginAddress
        end = entry.struct.EndAddress
        runtime_functions.append((begin, end))


def containing_function(rva: int):
    for begin, end in runtime_functions:
        if begin <= rva < end:
            return begin, end
    return None


text = next(
    section
    for section in pe.sections
    if section.Name.rstrip(b"\0") == b".text"
)
text_rva = text.VirtualAddress
text_data = text.get_data()


def direct_callers(target_rva: int):
    callers = []
    for index in range(len(text_data) - 5):
        if text_data[index] != 0xE8:
            continue
        displacement = struct.unpack_from("<i", text_data, index + 1)[0]
        source_rva = text_rva + index
        destination = source_rva + 5 + displacement
        if destination == target_rva:
            callers.append(source_rva)
    return callers


print(f"image_base=0x{image_base:X}")
print(f"exception_functions={len(runtime_functions)}")

seen_functions = set()
for target in TARGETS:
    function = containing_function(target)
    print(f"\n=== xref RVA +0x{target:X} function={function} ===")
    if not function:
        code = rva_bytes(target - 0x40, 0xA0)
        start = target - 0x40
    else:
        start, end = function
        code = rva_bytes(start, end - start)
        seen_functions.add(function)
    for insn in md.disasm(code, image_base + start):
        marker = ">>" if insn.address - image_base == target else "  "
        print(
            f"{marker} +0x{insn.address-image_base:08X} "
            f"{insn.mnemonic:8} {insn.op_str}"
        )
    if function:
        callers = direct_callers(function[0])
        print(
            "callers="
            + ", ".join(f"+0x{caller:X}" for caller in callers[:64])
        )

print("\n=== caller functions ===")
caller_functions = set()
for begin, _ in seen_functions:
    for caller in direct_callers(begin):
        function = containing_function(caller)
        if function:
            caller_functions.add(function)

for begin, end in sorted(caller_functions):
    print(f"\n--- function +0x{begin:X}..+0x{end:X} ---")
    code = rva_bytes(begin, min(end - begin, 0x500))
    for insn in md.disasm(code, image_base + begin):
        print(
            f"  +0x{insn.address-image_base:08X} "
            f"{insn.mnemonic:8} {insn.op_str}"
        )
