import ctypes
import pathlib
import struct
import sys
from ctypes import wintypes

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64


EXE = pathlib.Path(
    r"E:\SteamLibrary\steamapps\common\Monster Hunter World"
    r"\MonsterHunterWorld.exe"
)
TARGETS = [
    0x1F50E0,
    0x2032B0,
    0x203410,
    0x2035E0,
    0x2037C0,
    0x2038D0,
    0x203940,
    0x203A00,
    0x202FF0,
    0x213190,
    0x1FDA390,
    0x1FD57E0,
    0x1FDA863,
    0x22993DA,
    0x229943A,
    0x229A903,
    0x229EF70,
    0x229F0C4,
    0x2304967,
    0x242368E,
]
PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400

pid = int(sys.argv[1])
runtime_base = int(sys.argv[2], 0)
summary_only = len(sys.argv) > 3 and sys.argv[3] == "--summary"
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


def read_memory(address: int, size: int) -> bytes:
    buffer = ctypes.create_string_buffer(size)
    read = ctypes.c_size_t()
    if not kernel32.ReadProcessMemory(
        handle, ctypes.c_void_p(address), buffer, size, ctypes.byref(read)
    ):
        raise ctypes.WinError(ctypes.get_last_error())
    return buffer.raw[: read.value]


pe = pefile.PE(str(EXE), fast_load=False)
exception_entries = sorted(
    pe.DIRECTORY_ENTRY_EXCEPTION,
    key=lambda entry: entry.struct.BeginAddress,
)
functions = [
    (entry.struct.BeginAddress, entry.struct.EndAddress)
    for entry in exception_entries
]


def containing_function(rva: int):
    low = 0
    high = len(functions)
    while low < high:
        middle = (low + high) // 2
        begin, end = functions[middle]
        if rva < begin:
            high = middle
        elif rva >= end:
            low = middle + 1
        else:
            entry = exception_entries[middle]
            root = entry
            while getattr(root.unwindinfo, "_chained_entry", None):
                root = root.unwindinfo._chained_entry
            return root.struct.BeginAddress, end
    return None


text = next(
    section
    for section in pe.sections
    if section.Name.rstrip(b"\0") == b".text"
)
text_rva = text.VirtualAddress
text_size = max(text.Misc_VirtualSize, text.SizeOfRawData)
text_data = read_memory(runtime_base + text_rva, text_size)
md = Cs(CS_ARCH_X86, CS_MODE_64)

target_functions = {}
for target in TARGETS:
    function = containing_function(target)
    target_functions[target] = function

function_starts = {
    function[0] for function in target_functions.values() if function
}
callers_by_target = {start: [] for start in function_starts}
for index in range(0, len(text_data) - 5):
    if text_data[index] != 0xE8:
        continue
    displacement = struct.unpack_from("<i", text_data, index + 1)[0]
    source_rva = text_rva + index
    destination = source_rva + 5 + displacement
    if destination in callers_by_target:
        callers_by_target[destination].append(source_rva)

print(f"pid={pid} runtime_base=0x{runtime_base:X}")
print(f"text=+0x{text_rva:X} size=0x{text_size:X}")
for target in TARGETS:
    function = target_functions[target]
    print(f"\n=== target +0x{target:X}; function={function} ===")
    if summary_only:
        start, end = target - 0x50, target + 0x90
    elif not function:
        start, end = target - 0x60, target + 0xA0
    else:
        start, end = function
    code = read_memory(runtime_base + start, end - start)
    for instruction in md.disasm(code, runtime_base + start):
        rva = instruction.address - runtime_base
        marker = ">>" if rva == target else "  "
        print(
            f"{marker} +0x{rva:08X} "
            f"{instruction.mnemonic:8} {instruction.op_str}"
        )
    if function:
        callers = callers_by_target[function[0]]
        print(
            "callers="
            + ", ".join(f"+0x{caller:X}" for caller in callers[:128])
        )
        for caller in callers[:16]:
            caller_function = containing_function(caller)
            print(
                f"  caller +0x{caller:X} "
                f"in function {caller_function}"
            )

kernel32.CloseHandle(handle)
