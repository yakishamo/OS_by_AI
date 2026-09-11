#!/usr/bin/env python3
"""Run the serial-only x86_64 development machine; no third-party Python packages."""

import argparse
import os
from pathlib import Path
import selectors
import shutil
import subprocess
import sys
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"


def firmware(qemu):
    code = os.environ.get("OVMF_CODE")
    variables = os.environ.get("OVMF_VARS")
    if code or variables:
        if not code or not variables:
            raise RuntimeError("Set both OVMF_CODE and OVMF_VARS to a matching firmware pair.")
        pair = (Path(code).expanduser().resolve(), Path(variables).expanduser().resolve())
        if not all(path.is_file() for path in pair):
            raise RuntimeError("OVMF_CODE or OVMF_VARS does not point to a file.")
        return pair

    directories = [
        Path(qemu).resolve().parent.parent / "share/qemu",
        Path(qemu).parent.parent / "share/qemu",
        Path("/opt/homebrew/share/qemu"),
        Path("/usr/local/share/qemu"),
        Path("/usr/share/OVMF"),
        Path("/usr/share/edk2/ovmf"),
        Path("/usr/share/qemu"),
    ]
    names = [
        ("edk2-x86_64-code.fd", "edk2-i386-vars.fd"),
        ("OVMF_CODE_4M.fd", "OVMF_VARS_4M.fd"),
        ("OVMF_CODE.fd", "OVMF_VARS.fd"),
    ]
    for directory in directories:
        for code_name, vars_name in names:
            pair = (directory / code_name, directory / vars_name)
            if all(path.is_file() for path in pair):
                return pair
    raise RuntimeError("UEFI firmware not found. Set OVMF_CODE and OVMF_VARS; see README.md.")


def qemu_path():
    name = os.environ.get("QEMU", "qemu-system-x86_64")
    executable = shutil.which(name)
    if not executable:
        raise RuntimeError(f"QEMU executable not found: {name}")
    return executable


def drive_path(path):
    # QEMU's legacy -drive parser treats doubled commas as literal commas.
    return str(path).replace(",", ",,")


def command(qemu, code, variables, esp, mode):
    result = [
        qemu,
        "-machine", "q35",
        "-accel", "tcg",
        "-cpu", "qemu64",
        "-smp", "1",
        "-m", "256M",
        "-display", "none",
        "-nic", "none",
        "-no-reboot",
        "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={drive_path(code)}",
        "-drive", f"if=pflash,format=raw,unit=1,file={drive_path(variables)}",
        "-drive", f"if=none,id=esp,format=raw,readonly=on,file=fat:{drive_path(esp)}",
        "-device", "qemu-xhci,id=xhci",
        "-device", "usb-storage,drive=esp,bootindex=1",
    ]
    if mode == "debug":
        result += [
            "-monitor", "none",
            "-serial", f"file:{BUILD / 'serial-debug.log'}",
            "-S", "-gdb", "stdio",
        ]
    elif mode == "test":
        result += ["-monitor", "none", "-serial", "stdio"]
    else:
        result += ["-serial", "mon:stdio"]
    return result


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def smoke_test(arguments, timeout):
    log_path = BUILD / "serial-test.log"
    nonce = ("serial-" + uuid.uuid4().hex).encode("ascii")
    transcript = bytearray()
    stage = 0
    passed = False
    deadline = time.monotonic() + timeout
    with log_path.open("wb") as log, subprocess.Popen(
        arguments, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
    ) as process:
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                while time.monotonic() < deadline:
                    events = selector.select(timeout=min(0.2, max(0, deadline - time.monotonic())))
                    for key, _ in events:
                        data = os.read(key.fileobj.fileno(), 4096)
                        if not data:
                            raise RuntimeError("QEMU exited before the serial test completed.")
                        log.write(data)
                        log.flush()
                        transcript.extend(data)
                    outgoing = None
                    if b"BOOT ERROR:" in transcript:
                        raise RuntimeError("UEFI application reported an error; see serial log.")
                    if stage == 0 and b"BOOT: EFI_SERIAL_IO_PROTOCOL via LocateProtocol\r\n" in transcript and b"SERIAL> " in transcript:
                        outgoing = nonce + b"\r"
                    elif stage == 1 and nonce + b"\r\nSERIAL> " in transcript:
                        passed = True
                        break
                    if outgoing is not None:
                        process.stdin.write(outgoing)
                        process.stdin.flush()
                        transcript.clear()
                        stage += 1
                if not passed:
                    raise RuntimeError(f"UEFI Serial IO test timed out after {timeout:g}s (stage {stage}/1).")
        finally:
            stop(process)
            print(f"Serial log: {log_path}")
    print("PASS: UEFI Serial IO protocol discovery and serial input/output")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["run", "test", "debug", "doctor"])
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args()
    if not 0 < args.timeout < float("inf"):
        parser.error("--timeout must be positive and finite")

    qemu = qemu_path()
    code, variables = firmware(qemu)
    if args.mode == "doctor":
        subprocess.run([qemu, "--version"], check=True)
        print(f"Python: {sys.version.split()[0]}")
        print(f"QEMU: {qemu}")
        print(f"UEFI code: {code}")
        print(f"UEFI variable template: {variables}")
        print("Guest: x86_64 / q35 / TCG / 1 CPU / 256 MiB / COM1")
        return 0

    application = BUILD / "esp/EFI/BOOT/BOOTX64.EFI"
    if not application.is_file():
        raise RuntimeError("BOOTX64.EFI is missing; run make build first.")
    # Each VM owns a copy. Firmware templates and build output stay read-only.
    with tempfile.TemporaryDirectory(prefix="os-by-ai-") as temporary:
        directory = Path(temporary)
        vars_copy = directory / "vars.fd"
        shutil.copyfile(variables, vars_copy)
        esp = directory / "esp"
        boot_directory = esp / "EFI/BOOT"
        boot_directory.mkdir(parents=True)
        shutil.copyfile(application, boot_directory / application.name)
        arguments = command(qemu, code, vars_copy, esp, args.mode)
        if args.mode == "test":
            smoke_test(arguments, args.timeout)
        else:
            if args.mode == "debug":
                print(f"GDB on stdio; serial log: {BUILD / 'serial-debug.log'}", file=sys.stderr, flush=True)
            else:
                print("QEMU serial console: Ctrl-a x to quit; Ctrl-a c for monitor.", flush=True)
            with subprocess.Popen(arguments) as process:
                try:
                    return process.wait()
                finally:
                    stop(process)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, OSError, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(130)
