#!/usr/bin/env python3
"""Exercise the assembled ARM64 writer with a mocked Mach trap in Unicorn.

Usage: python3 tests/legacy-writer.py worker/legacy-syscalls-arm64.o
Requires Unicorn 2.1.2. No Darwin kernel, process, or device is contacted.
The persistent same-page restore failure test records a residual limitation;
it is not a claim that every permission failure is recoverable.
"""
import hashlib
import json
from pathlib import Path
import struct
import sys
import unittest

from unicorn import (Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE,
                     UC_HOOK_INTR, UC_ERR_FETCH_PROT, UC_PROT_READ,
                     UC_PROT_WRITE, UC_PROT_EXEC)
from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_X1,
                                UC_ARM64_REG_X2, UC_ARM64_REG_X3,
                                UC_ARM64_REG_X4, UC_ARM64_REG_X16,
                                UC_ARM64_REG_X30)


def load_writer(path):
    data = path.read_bytes()
    header = struct.unpack_from("<IiiIIIII", data)
    if header[0] != 0xFEEDFACF or header[1] != 0x0100000C or header[3] != 1:
        raise ValueError("expected a little-endian ARM64 Mach-O object")
    sections, symtab = [], None
    position = 32
    for _ in range(header[4]):
        command, size = struct.unpack_from("<II", data, position)
        if command == 0x19:
            count = struct.unpack_from("<I", data, position + 64)[0]
            for index in range(count):
                offset = position + 72 + index * 80
                address, length, file_offset = struct.unpack_from("<QQI", data, offset + 32)
                relocations = struct.unpack_from("<I", data, offset + 60)[0]
                sections.append((address, length, file_offset, relocations))
        elif command == 2:
            symtab = struct.unpack_from("<IIII", data, position + 8)
        position += size
    if symtab is None:
        raise ValueError("object has no symbol table")
    symoff, count, stroff, _ = symtab
    symbols = {}
    for index in range(count):
        name_offset, kind, section, _, value = struct.unpack_from("<IBBHQ", data, symoff + index * 16)
        if kind & 0xE0:
            continue
        end = data.index(b"\0", stroff + name_offset)
        name = data[stroff + name_offset:end].decode("ascii")
        symbols[name] = (section, value)
    section, start = symbols["_LHLegacyWriterStart"]
    end_section, end = symbols["_LHLegacyWriterEnd"]
    if section != end_section or not section or end <= start:
        raise ValueError("invalid writer bounds")
    address, length, offset, relocations = sections[section - 1]
    if relocations or start < address or end > address + length:
        raise ValueError("writer section has relocations or invalid bounds")
    code = data[offset + start - address:offset + end - address]
    if len(code) > 4096:
        raise ValueError("writer exceeds one page")
    return code, hashlib.sha256(data).hexdigest()


def execute_writer(page_size, protection=5, failures=(), same_page=False):
    code_address, destination, source, external_return = 0x100000, 0x200000, 0x300000, 0x400000
    return_address = destination + 0x300 if same_page else external_return
    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    cpu.mem_map(code_address, 0x1000, UC_PROT_READ | UC_PROT_EXEC)
    cpu.mem_write(code_address, WRITER)
    cpu.mem_map(destination, page_size, UC_PROT_READ | UC_PROT_WRITE)
    cpu.mem_map(source, page_size, UC_PROT_READ | UC_PROT_WRITE)
    cpu.mem_map(external_return, 0x1000, UC_PROT_READ | UC_PROT_EXEC)
    original = bytearray(b"\x35" * page_size)
    replacement = bytearray(b"\x91" * page_size)
    # Preserve a valid return instruction in both snapshots, though the hook
    # stops before executing it. An NX return must fail instruction fetch.
    if same_page:
        original[0x300:0x304] = replacement[0x300:0x304] = bytes.fromhex("c0035fd6")
    cpu.mem_write(destination, bytes(original))
    cpu.mem_write(source, bytes(replacement))
    cpu.mem_write(external_return, bytes.fromhex("c0035fd6"))
    cpu.mem_protect(destination, page_size, protection)
    trace, state = [], {"protection": protection, "returned": False}

    def trap(emulator, number, _):
        arguments = tuple(emulator.reg_read(reg) for reg in
                          (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2,
                           UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X16))
        assert arguments[:4] == (7, destination, page_size, 0), arguments
        assert arguments[5] == (1 << 64) - 14, arguments
        requested = arguments[4]
        trace.append(requested)
        result = 5 if len(trace) in failures else 0
        if not result:
            state["protection"] = requested & 7
            emulator.mem_protect(destination, page_size, requested & 7)
        emulator.reg_write(UC_ARM64_REG_X0, result)

    def instruction(emulator, address, size, _):
        if address == return_address:
            state["returned"] = True
            emulator.emu_stop()

    cpu.hook_add(UC_HOOK_INTR, trap)
    cpu.hook_add(UC_HOOK_CODE, instruction)
    for register, value in ((UC_ARM64_REG_X0, 7), (UC_ARM64_REG_X1, destination),
                            (UC_ARM64_REG_X2, source), (UC_ARM64_REG_X3, page_size),
                            (UC_ARM64_REG_X4, protection), (UC_ARM64_REG_X30, return_address)):
        cpu.reg_write(register, value)
    error = None
    try:
        cpu.emu_start(code_address, 0, timeout=1_000_000, count=100_000)
    except UcError as exception:
        error = exception.errno
    return {**state, "trace": trace, "result": cpu.reg_read(UC_ARM64_REG_X0),
            "bytes": bytes(cpu.mem_read(destination, page_size)),
            "original": bytes(original), "replacement": bytes(replacement), "error": error}


class LegacyWriterTests(unittest.TestCase):
    def test_success_preserves_permissions_and_full_page(self):
        for page in (4096, 16384):
            for protection in (1, 3, 5, 7):
                with self.subTest(page=page, protection=protection):
                    result = execute_writer(page, protection)
                    self.assertTrue(result["returned"])
                    self.assertIsNone(result["error"])
                    self.assertEqual(result["result"], 0)
                    self.assertEqual(result["trace"], [0x13, protection])
                    self.assertEqual(result["protection"], protection)
                    self.assertEqual(result["bytes"], result["replacement"])

    def test_same_page_success(self):
        for page in (4096, 16384):
            result = execute_writer(page, same_page=True)
            self.assertTrue(result["returned"])
            self.assertIsNone(result["error"])
            self.assertEqual(result["trace"], [0x13, 5])

    def test_first_protect_failure_changes_nothing(self):
        for page in (4096, 16384):
            result = execute_writer(page, failures=(1,), same_page=True)
            self.assertTrue(result["returned"])
            self.assertEqual(result["result"], 5)
            self.assertEqual(result["trace"], [0x13])
            self.assertEqual(result["protection"], 5)
            self.assertEqual(result["bytes"], result["original"])

    def test_transient_restore_failure_retries_before_same_page_return(self):
        for page in (4096, 16384):
            result = execute_writer(page, failures=(2,), same_page=True)
            self.assertTrue(result["returned"])
            self.assertIsNone(result["error"])
            self.assertEqual(result["trace"], [0x13, 5, 5])
            self.assertEqual(result["result"], 5)  # original failure drives rollback
            self.assertEqual(result["protection"], 5)
            self.assertEqual(result["bytes"], result["replacement"])

    def test_persistent_restore_failure_can_return_only_outside_target(self):
        result = execute_writer(4096, failures=(2, 3))
        self.assertTrue(result["returned"])
        self.assertEqual(result["result"], 5)
        self.assertEqual(result["trace"], [0x13, 5, 5])
        self.assertEqual(result["protection"], 3)

    def test_residual_persistent_same_page_restore_failure(self):
        result = execute_writer(4096, failures=(2, 3), same_page=True)
        self.assertFalse(result["returned"])
        self.assertEqual(result["error"], UC_ERR_FETCH_PROT)
        self.assertEqual(result["trace"], [0x13, 5, 5])
        self.assertEqual(result["protection"], 3)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: legacy-writer.py ARM64_MACHO_OBJECT")
    WRITER, object_digest = load_writer(Path(sys.argv.pop()))
    print(json.dumps({"object_sha256": object_digest, "writer_sha256": hashlib.sha256(WRITER).hexdigest(),
                      "writer_bytes": len(WRITER), "authority": "ARM64 emulation with mocked Mach traps",
                      "residual": "persistent same-page execute-permission restore failure remains unrecoverable"}))
    unittest.main(verbosity=2)
