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


def run_case(name, kernel, timeout, expected_error=None, memory_checks=()):
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
                        if b"BOOT ERROR:" in transcript:
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
                            if (rip and flags and "HLT=1" in regs and int(flags[1], 16) & 0x200 == 0
                                    and int(rip[1], 16) == symbols["kernel_halt"] + 1):
                                instruction = monitor(f"x /1i 0x{int(rip[1], 16) - 1:x}")
                                if not re.search(r"\bhlt\b", instruction):
                                    raise RuntimeError("Stopped outside HLT")

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
                                (log_dir / "boot-info.json").write_text(json.dumps({
                                    "address": pointer, "memory_map": mmap, "memory_map_size": mmap_size,
                                    "descriptor_size": stride, "descriptor_count": mmap_size // stride,
                                    "kernel_base": kernel_base, "kernel_size": kernel_size,
                                    "stack_base": stack_base, "stack_size": stack_size,
                                    "initial_rsp": initial_rsp, "observed_rsp": observed_rsp,
                                    "current_rsp": current_rsp, "boot_services_exited": True,
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
    data_offset = (len(fixture) + 4095) & ~4095
    payload = b"KERNEL-DATA-TEST!"
    fixture.extend(bytes(data_offset + len(payload) - len(fixture)))
    fixture[data_offset:] = payload
    struct.pack_into("<IIQQQQQQ", fixture, extra_ph,
                     1, 6, data_offset, 0x102000, 0x102000, len(payload), 8192, 4096)
    run_case("data-bss", fixture, timeout, memory_checks=[
        (0x102000, payload), (0x102000 + len(payload), bytes(16)), (0x103ff0, bytes(16))])
    overlap = bytearray(fixture)
    struct.pack_into("<QQ", overlap, extra_ph + 16, 0x100000, 0x100000)
    run_case("overlap", overlap, timeout, b"load kernel.elf status=0x8000000000000001")
