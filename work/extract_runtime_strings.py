import ctypes
import sys
from ctypes import wintypes

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP


pid = int(sys.argv[1])
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
base = int(sys.argv[2], 0)
start_rva = int(sys.argv[3], 0)
end_rva = int(sys.argv[4], 0)
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


def decode(raw):
    ascii_data = raw.split(b"\0", 1)[0]
    if len(ascii_data) >= 4 and all(
        32 <= value < 127 for value in ascii_data
    ):
        return ascii_data.decode("ascii")
    try:
        wide_end = raw.find(b"\0\0")
        if wide_end > 2:
            wide_end += wide_end % 2
            value = raw[:wide_end].decode("utf-16-le")
            if len(value) >= 3 and value.isprintable():
                return value
    except UnicodeDecodeError:
        pass
    return ""


code = read(base + start_rva, end_rva - start_rva)
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
seen = set()
for instruction in md.disasm(code, base + start_rva):
    for operand in instruction.operands:
        if operand.type != X86_OP_MEM or operand.mem.base != X86_REG_RIP:
            continue
        target = instruction.address + instruction.size + operand.mem.disp
        if target in seen:
            continue
        seen.add(target)
        value = decode(read(target, 160))
        if value:
            print(
                f"+0x{instruction.address-base:X} -> "
                f"+0x{target-base:X}: {value}"
            )

kernel32.CloseHandle(handle)
