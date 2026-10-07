#!/usr/bin/env python3
"""Skip one gate's delay in an exact, verified existing PIE task binary.

This intentionally supports only the six instruction layouts audited for the
2026-10-07 GCC build. A different compiler/layout fails its hash check.
"""

import hashlib
import struct
import subprocess
import sys
from pathlib import Path


# gate: (task, symbol, symbol VMA, delay start VMA, delay end VMA, SHA-256)
PATCHES = {
    "not": (3, "not", 0x4000, 0x401F, 0x4045,
            "10926c72993b4cac6b80a96d78571406d04fa9ba87a2af0313d0295921b6041c"),
    "nand": (4, "nand", 0x7C68, 0x7C89, 0x7CAF,
             "7af450872686ce8c95c997c89574fe4ab4db99d85a6651978215619ca012bcf5"),
    "nand2": (5, "nand2", 0x8B80, 0x8BA6, 0x8BE0,
              "378a6641d6240d6aa2c195fc94cccbf93642a4912700f62c98128b497c748c17"),
    "fan2": (6, "fan2_impl", 0x7D28, 0x7DFD, 0x7E4E,
             "3bf2590ec8e766a8a7a57050e58a4337ca88aedc85f1b5ce45ce937c92b41ea1"),
    "and": (6, "and_gate_impl", 0x8058, 0x813E, 0x8163,
            "a60733263192411008064ac8760ce3062db0fc2ead33d996b349661ffa1d9278"),
    "or": (6, "or_gate_impl", 0x8368, 0x8458, 0x8485,
           "ffff4cdaa53600323e90e7bd440d0ca8ddae9c0e00c52ab60a9f8828c7127b0d"),
}

BASELINE_SHA256 = {
    3: "0aa4a93bcf6f6698d698c48fd1287284bd6d0faab7275030024f62d04f03d48f",
    4: "406f46330cf3ef34cfab6b5faa460e75aaa3a2237aa41e3061e67550a99eb722",
    5: "b3a369019eda504fc4412e60613f4f7c62af98740b0cbe8feed4d5fc5af266a1",
    6: "8b3e81ea22840d238158f0224a057485ecf7b9ad2583b70a224ab16a4499ad6d",
}


def executable_file_offset(data: bytes, start: int, end: int) -> int:
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        raise ValueError("expected little-endian ELF64")
    elf_type, machine = struct.unpack_from("<HH", data, 16)
    if elf_type != 3 or machine != 62:
        raise ValueError("expected PIE x86-64 executable (ET_DYN)")
    phoff = struct.unpack_from("<Q", data, 32)[0]
    phentsize, phnum = struct.unpack_from("<HH", data, 54)
    for index in range(phnum):
        values = struct.unpack_from("<IIQQQQQQ", data, phoff + index * phentsize)
        kind, flags, file_offset, vaddr, _, file_size, _, _ = values
        if kind == 1 and flags & 1 and vaddr <= start < end <= vaddr + file_size:
            return file_offset + start - vaddr
    raise ValueError("delay span is not wholly inside an executable PT_LOAD")


def symbols(path: Path) -> bytes:
    return subprocess.check_output(["nm", "-an", str(path)])


def main() -> int:
    if len(sys.argv) != 4 or sys.argv[1] not in PATCHES:
        print("usage: patch_gate_delay_binary.py GATE BASELINE OUTPUT", file=sys.stderr)
        print("GATE is not, nand, nand2, fan2, and, or", file=sys.stderr)
        return 2
    gate, baseline, output = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
    task, symbol, symbol_vma, start, end, expected_hash = PATCHES[gate]
    if baseline.name != f"task{task}.out":
        raise ValueError(f"{gate} requires task{task}.out")
    if baseline.resolve() == output.resolve():
        raise ValueError("output must differ from baseline")

    original = baseline.read_bytes()
    if hashlib.sha256(original).hexdigest() != BASELINE_SHA256[task]:
        raise ValueError("whole baseline binary differs from the audited build")
    offset = executable_file_offset(original, start, end)
    length = end - start
    span = original[offset:offset + length]
    if hashlib.sha256(span).hexdigest() != expected_hash:
        raise ValueError("delay bytes differ from the audited build; refusing patch")
    original_symbols = symbols(baseline)
    expected_symbol = f"{symbol_vma:016x} T {symbol}".encode()
    if expected_symbol not in original_symbols.splitlines():
        raise ValueError(f"{symbol} is not at its audited address")

    displacement = length - 2
    if not 0 <= displacement <= 127:
        raise ValueError("delay span does not fit a short forward jump")
    replacement = bytes((0xEB, displacement)) + b"\x90" * (length - 2)
    patched = original[:offset] + replacement + original[offset + length:]
    if len(patched) != len(original):
        raise AssertionError("file size changed")
    if original[:offset] != patched[:offset] or original[offset + length:] != patched[offset + length:]:
        raise AssertionError("bytes outside delay span changed")

    with output.open("xb") as stream:
        stream.write(patched)
    output.chmod(baseline.stat().st_mode & 0o777)
    if symbols(output) != original_symbols:
        raise AssertionError("ELF symbols changed")
    print(f"{gate}: {symbol}@{symbol_vma:#x}; skipped [{start:#x},{end:#x}); "
          f"file size and all bytes outside the delay span unchanged")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
