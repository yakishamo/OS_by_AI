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
from boot_checks import check_successful_boot


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


def check_console(monitor, command_check):
    idle_regs = monitor("info registers")
    if int(re.search(r"RFL=([0-9a-fA-F]+)", idle_regs)[1], 16) & 0x200 == 0:
        raise RuntimeError("Console did not enable interrupts")
    first = command_check(b"ticks\r", b"ticks: ")
    time.sleep(0.15)
    second = command_check(b"ticks\r", b"ticks: ")
    t1 = int(re.search(rb"ticks: (\d+)", first)[1])
    t2 = int(re.search(rb"ticks: (\d+)", second)[1])
    if not 0 < t1 < t2:
        raise RuntimeError("Timer interrupts did not recur")
    command_check(b"help\r\n", b"help  - list commands\r\n")
    command_check(b"hex\x08lp\n", b"help  - list commands\r\n")
    command_check(b"hex\x7flp\r", b"help  - list commands\r\n")
    command_check(b"\x1b[A\x1bOB help \t\r", b"help  - list commands\r\n")
    command_check(b"\x08\x7f\r", b"\r\nK> ")
    command_check(b"halt\x03", b"^C\r\nK> ")
    command_check(b"unknown\r", b"Unknown command.")
    command_check(b"help extra\r", b"Unknown command.")
    command_check(b" " * 127 + b"\r", b"\r\nK> ")
    command_check(b"help" + b" " * 124 + b"\r", b"Input discarded")
    reply = command_check(b"mem\r", b"pages: total=")
    counts = re.search(rb"pages: total=(\d+) used=(\d+) free=(\d+)", reply)
    if not counts or int(counts[1]) != int(counts[2]) + int(counts[3]) or int(counts[3]) == 0:
        raise RuntimeError("Invalid console memory counts")
    command_check(b"clear\r", b"\x1b[2J\x1b[H")
    print("PASS: console commands, editing, CRLF, cancellation and length limits", flush=True)


def check_exception(name, symbols, regs, serial, expected_exception):
    rip = re.search(r"RIP=([0-9a-fA-F]+)", regs)
    flags = re.search(r"RFL=([0-9a-fA-F]+)", regs)
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
        serial_pipe = directory / "uart"
        os.mkfifo(str(serial_pipe) + ".in")
        os.mkfifo(str(serial_pipe) + ".out")
        command += ["-chardev", f"pipe,id=uart,path={serial_pipe},logfile={serial}",
                    "-serial", "chardev:uart"]
        with (log_dir / "qemu.log").open("wb") as errors, subprocess.Popen(
            command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors
        ) as process:
            connection = None
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
                    console_driven = False
                    while time.monotonic() < deadline:
                        if process.poll() is not None:
                            raise RuntimeError(f"{name}: QEMU exited")
                        transcript = serial.read_bytes()
                        if not console_driven and b"CONSOLE: ready (type 'help')\r\nK> " in transcript:
                            connection = os.fdopen(os.open(str(serial_pipe) + ".in", os.O_RDWR | os.O_NONBLOCK), "wb", buffering=0)

                            def send(data):
                                # Respect the polled UART's small receive FIFO, including echo time.
                                for byte in data:
                                    connection.write(bytes([byte]))
                                    time.sleep(0.003)

                            def command_check(data, expected):
                                start = len(serial.read_bytes())
                                send(data)
                                while time.monotonic() < deadline:
                                    reply = serial.read_bytes()[start:]
                                    if reply.endswith(b"K> "):
                                        if expected not in reply or reply.count(b"K> ") != 1:
                                            raise RuntimeError(f"{name}: console reply mismatch: {reply!r}")
                                        return reply
                                    time.sleep(0.01)
                                raise RuntimeError(f"{name}: console command timed out")

                            if name == "kernel":
                                check_console(monitor, command_check)
                            send(b"halt\r")
                            console_driven = True
                            continue
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
                            if expected_exception and rip and flags and "HLT=1" in regs and int(flags[1], 16) & 0x200 == 0:
                                check_exception(name, symbols, regs, serial, expected_exception)
                                return
                            if (rip and flags and "HLT=1" in regs and int(flags[1], 16) & 0x200 == 0
                                    and int(rip[1], 16) == symbols["kernel_halt"] + 1):
                                check_successful_boot(name, kernel, symbols, regs, monitor,
                                                      serial, log_dir, memory_checks)
                                return
                        time.sleep(0.1)
                    raise RuntimeError(f"{name}: timeout; logs in {log_dir}")
            finally:
                if connection is not None:
                    connection.close()
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
    for name, symbol, vector, error, target in (
        ("text-write", "paging_test_text_write", 14, 3, symbols["kernel_text_start"]),
        ("rodata-write", "paging_test_rodata_write", 14, 3, symbols["kernel_rodata_start"]),
        ("data-exec", "paging_test_data_exec", 14, 17, symbols["nx_data"]),
        ("stack-exec", "paging_test_stack_exec", 14, 17, None),
        ("stack-guard-low", "paging_test_stack_low", 14, 0, None),
        ("stack-guard-high", "paging_test_stack_high", 14, 0, None),
        ("df-guard-low", "paging_test_df_low", 14, 0, symbols["double_fault_guard_low"]),
        ("df-guard-high", "paging_test_df_high", 14, 0, symbols["double_fault_guard_high"]),
        ("stack-overflow", "paging_test_stack_overflow", 8, 0, None),
    ):
        jump = b"\xe9" + struct.pack("<i", symbols[symbol] - symbols["kernel_halt"] - 5)
        run_case(name, fault_image(jump), timeout, expected_exception=(vector, error, None, target))
