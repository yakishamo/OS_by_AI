"""Read-only checks of a halted kernel's tables, memory and boot contract."""
import json
import re
import struct
import time


def check_descriptor_tables(regs, symbols, read_memory):
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


def check_page_tables(regs, symbols, read_memory, read_u64, covered,
                      descriptors, stride, stack_base, stack_size):
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
            physical = value & 0x000ffffffffff000
            virtual = prefix | (index << shift)
            expected_flags = 3
            if shift == 12:
                expected_flags = 3 | (1 << 63)
                if symbols["kernel_text_start"] <= virtual < symbols["kernel_text_end"]:
                    expected_flags = 1
                elif symbols["kernel_rodata_start"] <= virtual < symbols["kernel_rodata_end"]:
                    expected_flags = 1 | (1 << 63)
            # Only accessed/dirty may be changed by the CPU.
            if (value & ~0x000ffffffffff000 & ~0x60) != expected_flags:
                raise RuntimeError("Invalid paging protection or unexpected huge page")
            if shift == 12:
                if virtual != physical:
                    raise RuntimeError("Non-identity mapping")
                mapped.add(virtual)
            else:
                walk(physical, shift - 9, virtual)

    walk(root, 39, 0)
    expected_pages = set()
    for offset in range(0, len(descriptors), stride):
        kind, _, physical, _, pages, attr = struct.unpack_from("<IIQQQQ", descriptors, offset)
        if kind in (1, 2, 7) and attr & 8 and not attr & (1 << 63):
            expected_pages.update(range(max(physical, 0x100000),
                                        min(physical + pages * 4096, 1 << 32), 4096))
    guards = {stack_base - 4096, stack_base + stack_size,
              symbols["double_fault_guard_low"], symbols["double_fault_guard_high"]}
    if (symbols["double_fault_stack"] != symbols["double_fault_guard_low"] + 4096
            or symbols["double_fault_guard_high"] != symbols["double_fault_stack"] + 16384
            or len(guards) != 4 or any(g % 4096 for g in guards)):
        raise RuntimeError("Invalid stack guard layout")
    expected_pages -= guards
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
    return cr3, old_cr3, len(tables), len(mapped)


def check_successful_boot(name, kernel, symbols, regs, monitor, serial, log_dir, memory_checks):
    instruction = monitor(f"x /1i 0x{symbols['kernel_halt']:x}")
    if not re.search(r"\bhlt\b", instruction):
        raise RuntimeError("Stopped outside HLT")
    kernel_log = (
        b"KERNEL: serial ready (COM1, 115200 8N1)\r\n"
        b"KERNEL: boot information verified\r\n"
        b"KERNEL: GDT/IDT/TSS ready\r\n"
        b"KERNEL: physical pages ready (4 KiB, self-test passed)\r\n"
        b"KERNEL: paging ready (own CR3, RAM check passed)\r\n"
        b"KERNEL: dynamic paging checks passed\r\n"
        b"KERNEL: timer ready (PIT, ~100 Hz)\r\n"
        b"KERNEL: serial receive ready (IRQ4, 255-byte buffer)\r\n"
        b"CONSOLE: ready (type 'help')\r\nK> ")
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

    halted_ticks = read_u64(symbols["ticks"])
    time.sleep(0.03)
    if halted_ticks == 0 or read_u64(symbols["ticks"]) != halted_ticks:
        raise RuntimeError("Timer missing or halt still accepts interrupts")
    if read_u64(symbols["unexpected_irqs"]):
        raise RuntimeError("Unexpected hardware IRQ")
    if not read_u64(symbols["serial_rx_interrupts"]):
        raise RuntimeError("No UART receive interrupt delivered")

    check_descriptor_tables(regs, symbols, read_memory)
    pointer = read_u64(symbols["kernel_boot_info"])
    info = struct.unpack("<QIIQQQIIQQQQQ", read_memory(pointer, 88))
    (magic, version, info_size, mmap, mmap_size, stride, desc_version,
     reserved, kernel_base, kernel_size, stack_base, stack_size, boot_flags) = info
    initial_rsp = read_u64(symbols["kernel_initial_rsp"])
    observed_rsp = read_u64(symbols["kernel_observed_rsp"])
    current_rsp = int(re.search(r"RSP=([0-9a-fA-F]+)", regs)[1], 16)
    if (magic, version, info_size) != (0x4f5342494e464f31, 2, 88):
        raise RuntimeError("Invalid boot information identity")
    if (reserved, boot_flags, desc_version) != (0, 1, 1):
        raise RuntimeError("Invalid boot information version/flags")
    if not pointer or pointer % 4096 or mmap != pointer + 4096:
        raise RuntimeError("Invalid handoff placement")
    if not 0 < mmap_size <= 65536 or stride < 40:
        raise RuntimeError("Invalid memory map dimensions")
    if mmap_size % stride:
        raise RuntimeError("Incomplete memory descriptor")
    if stack_size != 65536 or stack_base != mmap + 65536 + 4096:
        raise RuntimeError("Invalid stack extent")
    if initial_rsp != stack_base + stack_size or initial_rsp % 16:
        raise RuntimeError("Invalid initial stack pointer")
    if not stack_base <= observed_rsp < initial_rsp:
        raise RuntimeError("C entry stack outside allocation")
    if not stack_base <= current_rsp < initial_rsp:
        raise RuntimeError("Halted stack outside allocation")
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
            or not covered(pointer, 35 * 4096, 2)
            or not (pointer + 35 * 4096 <= low or pointer >= high)):
        raise RuntimeError("Kernel / handoff ownership missing from final memory map")
    cr3, old_cr3, table_count, mapped_count = check_page_tables(
        regs, symbols, read_memory, read_u64, covered, descriptors, stride, stack_base, stack_size)
    (log_dir / "boot-info.json").write_text(json.dumps({
        "address": pointer, "memory_map": mmap, "memory_map_size": mmap_size,
        "descriptor_size": stride, "descriptor_count": mmap_size // stride,
        "kernel_base": kernel_base, "kernel_size": kernel_size,
        "stack_base": stack_base, "stack_size": stack_size,
        "initial_rsp": initial_rsp, "observed_rsp": observed_rsp,
        "current_rsp": current_rsp, "boot_services_exited": True,
        "cr3": cr3, "previous_cr3": old_cr3,
        "page_tables": table_count, "mapped_pages": mapped_count,
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
