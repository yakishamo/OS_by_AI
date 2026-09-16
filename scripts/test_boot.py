"""QEMU integration checks, including malformed input and segment initialization."""
import json
import os
from pathlib import Path
import re
import selectors
import shutil
import struct
import subprocess
import tempfile
import time

import qemu


def elf_symbols(data):
    """Read ELF symbols without requiring a host binutils installation."""
    shoff = struct.unpack_from("<Q", data, 40)[0]
    shsize, shnum = struct.unpack_from("<HH", data, 58)
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shsize) for i in range(shnum)]
    symbols = {}
    for section in sections:
        if section[1] != 2:
            continue
        strings_section = sections[section[6]]
        strings = data[strings_section[4]:strings_section[4] + strings_section[5]]
        for offset in range(section[4], section[4] + section[5], section[9]):
            name, _, _, _, value, _ = struct.unpack_from("<IBBHQQ", data, offset)
            symbols[strings[name:].split(b"\0", 1)[0].decode()] = value
    return symbols


def run_case(name, kernel, timeout, expected_error=None, memory_checks=(), expected_exception=None):
    executable = qemu.qemu_path()
    code, variables = qemu.firmware(executable)
    log_dir = qemu.BUILD / "tests" / name
    log_dir.mkdir(parents=True, exist_ok=True)
    serial = log_dir / "serial.log"
    serial.write_bytes(b"")
    deadline = time.monotonic() + timeout
    with tempfile.TemporaryDirectory(prefix="os-kernel-test-") as temporary:
        directory = Path(temporary)
        esp = directory / "esp"
        boot = esp / "EFI/BOOT"
        boot.mkdir(parents=True)
        shutil.copyfile(qemu.BUILD / "esp/EFI/BOOT/BOOTX64.EFI", boot / "BOOTX64.EFI")
        if kernel is not None:
            (esp / "kernel.elf").write_bytes(kernel)
        shutil.copyfile(variables, directory / "vars.fd")
        command = qemu.command(executable, code, directory / "vars.fd", esp, "test")
        command += ["-serial", f"file:{serial}"]
        with (log_dir / "qemu.log").open("wb") as errors, subprocess.Popen(
            command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors
        ) as process:
            try:
                with selectors.DefaultSelector() as selector:
                    selector.register(process.stdout, selectors.EVENT_READ)
                    pending = bytearray()

                    def receive():
                        while time.monotonic() < deadline:
                            if b"\n" in pending:
                                line, _, rest = pending.partition(b"\n")
                                pending[:] = rest
                                return json.loads(line)
                            if selector.select(0.1):
                                data = os.read(process.stdout.fileno(), 4096)
                                if not data:
                                    raise RuntimeError(f"{name}: QEMU exited")
                                pending.extend(data)
                        raise RuntimeError(f"{name}: timeout; logs in {log_dir}")

                    def request(command, arguments=None):
                        payload = {"execute": command}
                        if arguments is not None:
                            payload["arguments"] = arguments
                        process.stdin.write(json.dumps(payload).encode() + b"\n")
                        process.stdin.flush()
                        while True:
                            response = receive()
                            if "error" in response:
                                raise RuntimeError(str(response["error"]))
                            if "return" in response:
                                return response["return"]

                    def monitor(command):
                        return request("human-monitor-command", {"command-line": command})

                    if "QMP" not in receive():
                        raise RuntimeError("Missing QMP greeting")
                    request("qmp_capabilities")
                    while time.monotonic() < deadline:
                        if process.poll() is not None:
                            raise RuntimeError(f"{name}: QEMU exited")
                        transcript = serial.read_bytes()
                        if re.search(rb"BOOT ERROR:[^\r\n]*\r\n", transcript):
                            if expected_error and expected_error in transcript and b"BOOT: kernel entry=" not in transcript:
                                print(f"PASS: {name} rejected by loader", flush=True)
                                return
                            raise RuntimeError(f"{name}: unexpected boot error; see {serial}")
                        entry = re.search(rb"BOOT: kernel entry=0x([0-9a-f]{16})\r\n", transcript)
                        if entry:
                            if expected_error:
                                raise RuntimeError(f"{name}: invalid kernel was accepted")
                            regs = monitor("info registers")
                            (log_dir / "cpu.log").write_text(regs)
                            rip = re.search(r"RIP=([0-9a-fA-F]+)", regs)
                            flags = re.search(r"RFL=([0-9a-fA-F]+)", regs)
                            symbols = elf_symbols(kernel)
                            if expected_exception and rip and "HLT=1" in regs:
                                if int(rip[1], 16) != symbols["exception_halt"] + 2:
                                    raise RuntimeError(f"{name}: did not reach exception halt")
                                vector, error, rip_delta, cr2 = expected_exception
                                output = serial.read_bytes()
                                required = {"vector": vector, "error": error, "cs": 8}
                                if rip_delta is not None:
                                    required["rip"] = symbols["kernel_halt"] + rip_delta
                                if cr2 is not None:
                                    required["cr2"] = cr2
                                for field, value in required.items():
                                    if f"{field}=0x{value:016x}\r\n".encode() not in output:
                                        raise RuntimeError(f"{name}: invalid exception {field}; see {serial}")
                                if not flags or int(flags[1], 16) & 0x200:
                                    raise RuntimeError("Exception handler enabled interrupts")
                                if vector == 8:
                                    handler_rsp = int(re.search(r"RSP=([0-9a-fA-F]+)", regs)[1], 16)
                                    bottom = symbols["double_fault_stack"]
                                    if not bottom <= handler_rsp < bottom + 16384:
                                        raise RuntimeError("Double fault did not use IST stack")
                                print(f"PASS: {name}: exception vector/error/RIP and halt verified", flush=True)
                                return
                            if (rip and flags and "HLT=1" in regs and int(flags[1], 16) & 0x200 == 0
                                    and int(rip[1], 16) == symbols["kernel_halt"] + 1):
                                instruction = monitor(f"x /1i 0x{int(rip[1], 16) - 1:x}")
                                if not re.search(r"\bhlt\b", instruction):
                                    raise RuntimeError("Stopped outside HLT")
                                kernel_log = (
                                    b"KERNEL: serial ready (COM1, 115200 8N1)\r\n"
                                    b"KERNEL: boot information verified\r\n"
                                    b"KERNEL: GDT/IDT/TSS ready\r\n"
                                    b"KERNEL: physical pages ready (4 KiB, self-test passed)\r\n"
                                    b"KERNEL: paging ready (own CR3, RAM check passed)\r\n"
                                    b"KERNEL: dynamic paging checks passed\r\n"
                                    b"KERNEL: halting\r\n")
                                if kernel_log not in serial.read_bytes():
                                    raise RuntimeError(f"{name}: kernel serial output missing or corrupted")

                                def read_memory(address, length):
                                    result = bytearray()
                                    while len(result) < length:
                                        count = min(256, length - len(result))
                                        dump = monitor(f"xp /{count}bx 0x{address + len(result):x}")
                                        values = []
                                        for line in dump.splitlines():
                                            if ":" in line:
                                                values.extend(int(x, 16) for x in re.findall(
                                                    r"0x([0-9a-fA-F]{2})\b", line.split(":", 1)[1]))
                                        if len(values) != count:
                                            raise RuntimeError(f"Incomplete memory read: {dump}")
                                        result.extend(values)
                                    return bytes(result)

                                def read_u64(address):
                                    return struct.unpack("<Q", read_memory(address, 8))[0]

                                for table, symbol, limit in (("GDT", "kernel_gdt", 39), ("IDT", "kernel_idt", 4095)):
                                    descriptor = re.search(rf"{table}=\s*([0-9a-fA-F]+)\s+([0-9a-fA-F]+)", regs)
                                    if not descriptor or (int(descriptor[1], 16), int(descriptor[2], 16)) != (symbols[symbol], limit):
                                        raise RuntimeError(f"Incorrect {table} register")
                                for segment, segment_selector in (("CS", 8), ("SS", 16), ("DS", 16), ("ES", 16), ("TR", 24)):
                                    value = re.search(rf"{segment}\s*=([0-9a-fA-F]+)", regs)
                                    if not value or int(value[1], 16) != segment_selector:
                                        raise RuntimeError(f"Incorrect {segment} selector")
                                gates = read_memory(symbols["kernel_idt"], 4096)
                                for vector in range(256):
                                    lo, gate_selector, ist, attr, mid, hi, reserved = struct.unpack_from("<HHBBHII", gates, vector * 16)
                                    if ((lo | mid << 16 | hi << 32) != symbols[f"isr_{vector}"]
                                            or gate_selector != 8 or ist != (1 if vector == 8 else 0)
                                            or attr != 0x8e or reserved):
                                        raise RuntimeError(f"Invalid IDT gate {vector}")
                                tss = read_memory(symbols["kernel_tss"], 104)
                                if (struct.unpack_from("<Q", tss, 36)[0] != symbols["double_fault_stack"] + 16384
                                        or struct.unpack_from("<H", tss, 102)[0] != 104):
                                    raise RuntimeError("Invalid TSS / IST configuration")
                                pointer = read_u64(symbols["kernel_boot_info"])
                                info = struct.unpack("<QIIQQQIIQQQQQ", read_memory(pointer, 88))
                                (magic, version, info_size, mmap, mmap_size, stride, desc_version,
                                 reserved, kernel_base, kernel_size, stack_base, stack_size, boot_flags) = info
                                initial_rsp = read_u64(symbols["kernel_initial_rsp"])
                                observed_rsp = read_u64(symbols["kernel_observed_rsp"])
                                current_rsp = int(re.search(r"RSP=([0-9a-fA-F]+)", regs)[1], 16)
                                if (magic != 0x4f5342494e464f31 or version != 1 or info_size != 88
                                        or reserved != 0 or boot_flags != 1 or desc_version != 1
                                        or not pointer or pointer % 4096 or mmap != pointer + 4096
                                        or not 0 < mmap_size <= 65536 or stride < 40 or mmap_size % stride
                                        or stack_size != 65536 or stack_base != mmap + 65536
                                        or initial_rsp != stack_base + stack_size or initial_rsp % 16
                                        or not stack_base <= observed_rsp < initial_rsp
                                        or not stack_base <= current_rsp < initial_rsp):
                                    raise RuntimeError(f"{name}: invalid boot information or stack: {info}")
                                phoff = struct.unpack_from("<Q", kernel, 32)[0]
                                phnum = struct.unpack_from("<H", kernel, 56)[0]
                                segments = [struct.unpack_from("<IIQQQQQQ", kernel, phoff + i * 56)
                                            for i in range(phnum)]
                                segments = [p for p in segments if p[0] == 1 and p[6]]
                                low = min(p[3] & ~4095 for p in segments)
                                high = max((p[3] + p[6] + 4095) & ~4095 for p in segments)
                                if (kernel_base, kernel_size) != (low, high - low):
                                    raise RuntimeError("Boot information kernel extent mismatch")
                                descriptors = read_memory(mmap, mmap_size)
                                ranges = []
                                for offset in range(0, mmap_size, stride):
                                    kind, _, physical, _, pages, _ = struct.unpack_from("<IIQQQQ", descriptors, offset)
                                    ranges.append((physical, physical + pages * 4096, kind))

                                def covered(start, length, kind):
                                    cursor = start
                                    for begin, end, memory_type in sorted(ranges):
                                        if memory_type == kind and begin <= cursor < end:
                                            cursor = end
                                        if cursor >= start + length:
                                            return True
                                    return False

                                if (not covered(kernel_base, kernel_size, 1)
                                        or not covered(pointer, 33 * 4096, 2)
                                        or not (pointer + 33 * 4096 <= low or pointer >= high)):
                                    raise RuntimeError("Kernel / handoff ownership missing from final memory map")
                                root = read_u64(symbols["paging_root"])
                                old_cr3 = read_u64(symbols["paging_previous_cr3"])
                                cr3 = int(re.search(r"CR3=([0-9a-fA-F]+)", regs)[1], 16)
                                if cr3 != root or root == (old_cr3 & ~4095) or root % 4096:
                                    raise RuntimeError("Kernel CR3 was not replaced")
                                tables, mapped = set(), set()

                                def walk(table, shift, prefix):
                                    if table in tables or not covered(table, 4096, 7):
                                        raise RuntimeError("Page table reused or outside conventional RAM")
                                    tables.add(table)
                                    entries = struct.unpack("<512Q", read_memory(table, 4096))
                                    for index, value in enumerate(entries):
                                        if not value:
                                            continue
                                        # Permit CPU-updated accessed/dirty bits only.
                                        if (value & ~0x000ffffffffff000 & ~0x60) != 3:
                                            raise RuntimeError("Invalid paging flags or unexpected huge page")
                                        physical = value & 0x000ffffffffff000
                                        virtual = prefix | (index << shift)
                                        if shift == 12:
                                            if virtual != physical:
                                                raise RuntimeError("Non-identity mapping")
                                            mapped.add(virtual)
                                        else:
                                            walk(physical, shift - 9, virtual)

                                walk(root, 39, 0)
                                expected_pages = set()
                                for offset in range(0, mmap_size, stride):
                                    kind, _, physical, _, pages, attr = struct.unpack_from("<IIQQQQ", descriptors, offset)
                                    if kind in (1, 2, 7) and attr & 8 and not attr & (1 << 63):
                                        expected_pages.update(range(max(physical, 0x100000),
                                                                    min(physical + pages * 4096, 1 << 32), 4096))
                                if mapped != expected_pages or not tables <= mapped:
                                    raise RuntimeError("RAM coverage or reserved-region exclusion mismatch")
                                if len(tables) != read_u64(symbols["paging_table_count"]):
                                    raise RuntimeError("Page table accounting mismatch")
                                if read_u64(symbols["total_pages"]) - read_u64(symbols["free_pages"]) != len(tables):
                                    raise RuntimeError("PMM allocation count does not match page tables")
                                for table in tables:
                                    page = table // 4096
                                    if not read_u64(symbols["allocated"] + (page // 64) * 8) & (1 << (page % 64)):
                                        raise RuntimeError("Active page table is not owned by PMM")
                                (log_dir / "boot-info.json").write_text(json.dumps({
                                    "address": pointer, "memory_map": mmap, "memory_map_size": mmap_size,
                                    "descriptor_size": stride, "descriptor_count": mmap_size // stride,
                                    "kernel_base": kernel_base, "kernel_size": kernel_size,
                                    "stack_base": stack_base, "stack_size": stack_size,
                                    "initial_rsp": initial_rsp, "observed_rsp": observed_rsp,
                                    "current_rsp": current_rsp, "boot_services_exited": True,
                                    "cr3": cr3, "previous_cr3": old_cr3,
                                    "page_tables": len(tables), "mapped_pages": len(mapped),
                                }, indent=2) + "\n")
                                for address, expected in memory_checks:
                                    dump = monitor(f"xp /{len(expected)}bx 0x{address:x}")
                                    values = []
                                    for line in dump.splitlines():
                                        if ":" in line:
                                            values.extend(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})\b", line.split(":", 1)[1]))
                                    if bytes(values) != expected:
                                        raise RuntimeError(f"{name}: memory mismatch at {address:#x}: {dump}")
                                with (log_dir / "cpu.log").open("a") as log:
                                    log.write("\n" + instruction)
                                print(f"PASS: {name}: kernel HLT, boot information, dedicated stack and segments verified", flush=True)
                                return
                        time.sleep(0.1)
                    raise RuntimeError(f"{name}: timeout; logs in {log_dir}")
            finally:
                qemu.stop(process)


def test_all(timeout=60):
    original = (qemu.BUILD / "esp/kernel.elf").read_bytes()
    run_case("kernel", original, timeout)
    run_case("missing", None, timeout, b"load kernel.elf status=0x800000000000000e")
    bad_magic = bytearray(original)
    bad_magic[0] = 0
    run_case("bad-magic", bad_magic, timeout, b"load kernel.elf status=0x8000000000000001")
    phoff = struct.unpack_from("<Q", original, 32)[0]
    truncated = bytearray(original)
    struct.pack_into("<Q", truncated, phoff + 8, len(original) + 1)
    run_case("bad-segment", truncated, timeout, b"load kernel.elf status=0x8000000000000001")
    bad_entry = bytearray(original)
    struct.pack_into("<Q", bad_entry, 24, 0x80000)
    run_case("bad-entry", bad_entry, timeout, b"load kernel.elf status=0x8000000000000001")

    # A real two-segment ELF fixture: keep the kernel text, add data and BSS.
    fixture = bytearray(original)
    phnum = struct.unpack_from("<H", original, 56)[0]
    struct.pack_into("<H", fixture, 56, phnum + 1)
    extra_ph = phoff + 56 * phnum
    original_segments = [struct.unpack_from("<IIQQQQQQ", original, phoff + 56 * i)
                         for i in range(phnum)]
    data_address = max((p[3] + p[6] + 4095) & ~4095
                       for p in original_segments if p[0] == 1 and p[6])
    data_offset = (len(fixture) + 4095) & ~4095
    payload = b"KERNEL-DATA-TEST!"
    fixture.extend(bytes(data_offset + len(payload) - len(fixture)))
    fixture[data_offset:] = payload
    struct.pack_into("<IIQQQQQQ", fixture, extra_ph,
                     1, 6, data_offset, data_address, data_address, len(payload), 8192, 4096)
    run_case("data-bss", fixture, timeout, memory_checks=[
        (data_address, payload), (data_address + len(payload), bytes(16)),
        (data_address + 8192 - 16, bytes(16))])
    overlap = bytearray(fixture)
    struct.pack_into("<QQ", overlap, extra_ph + 16, 0x100000, 0x100000)
    run_case("overlap", overlap, timeout, b"load kernel.elf status=0x8000000000000001")

    symbols = elf_symbols(original)
    def fault_image(instructions):
        image = bytearray(original)
        address = symbols["kernel_halt"]
        for segment in original_segments:
            if segment[0] == 1 and segment[3] <= address and address + len(instructions) <= segment[3] + segment[5]:
                offset = segment[2] + address - segment[3]
                image[offset:offset + len(instructions)] = instructions
                return image
        raise RuntimeError("Fault fixture does not fit kernel text")

    run_case("invalid-opcode", fault_image(bytes.fromhex("0f0b")), timeout,
             expected_exception=(6, 0, 0, None))
    # Select just beyond our GDT; avoid depending on the inherited LDT mapping.
    run_case("general-protection", fault_image(bytes.fromhex("6a28588ed8")), timeout,
             expected_exception=(13, 0x28, 3, None))
    # Read from 1 TiB, outside the kernel's own page tables.
    run_case("page-fault", fault_image(bytes.fromhex("6a015848c1e028488b00")), timeout,
             expected_exception=(14, 0, 7, 1 << 40))
    run_case("null-page", fault_image(bytes.fromhex("31c0488b00")), timeout,
             expected_exception=(14, 0, 2, 0))
    # Destroy RSP, then push: delivering the resulting fault also fails.
    run_case("double-fault", fault_image(bytes.fromhex("31e450")), timeout,
             expected_exception=(8, 0, None, None))
    for name, symbol, error in (("readonly", "paging_test_readonly", 3),
                                ("nx", "paging_test_nx", 17),
                                ("unmapped", "paging_test_unmapped", 0)):
        jump = b"\xe9" + struct.pack("<i", symbols[symbol] - symbols["kernel_halt"] - 5)
        run_case(name, fault_image(jump), timeout,
                 expected_exception=(14, error, None, 0xffff800000000000))
