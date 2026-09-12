"""Check the independent loader and kernel artifacts without booting a kernel."""
from pathlib import Path
import struct

root = Path(__file__).resolve().parent.parent / "build/esp"


def require(condition, message):
    if not condition:
        raise SystemExit("FAIL: " + message)


pe = (root / "EFI/BOOT/BOOTX64.EFI").read_bytes()
require(pe[:2] == b"MZ", "loader DOS header")
offset = struct.unpack_from("<I", pe, 0x3c)[0]
require(pe[offset:offset + 4] == b"PE\0\0", "loader PE signature")
require(struct.unpack_from("<H", pe, offset + 4)[0] == 0x8664, "loader x86_64 machine")
optional = offset + 24
require(struct.unpack_from("<H", pe, optional)[0] == 0x20b, "loader PE32+")
require(struct.unpack_from("<H", pe, optional + 68)[0] == 10, "loader EFI application subsystem")

elf = (root / "kernel.elf").read_bytes()
require(elf[:6] == b"\x7fELF\x02\x01", "kernel little-endian ELF64")
kind, machine, version, entry, phoff = struct.unpack_from("<HHIQQ", elf, 16)
require((kind, machine, version) == (2, 62, 1), "kernel x86_64 executable")
phsize, phnum = struct.unpack_from("<HH", elf, 54)
require(phsize == 56 and phnum > 0, "kernel program headers")
executable_entry = False
for index in range(phnum):
    kind, flags, offset, virtual, physical, filesz, memsz, align = struct.unpack_from(
        "<IIQQQQQQ", elf, phoff + index * phsize)
    require(kind not in (2, 3), "kernel must not need a dynamic linker")
    if kind == 1:
        require(filesz <= memsz and offset + filesz <= len(elf), "kernel segment bounds")
        require(virtual == physical, "initial kernel uses identity load addresses")
        if flags & 1 and virtual <= entry < virtual + filesz:
            executable_entry = True
require(executable_entry, "kernel entry must be in a file-backed executable segment")
print(f"PASS: separate x86_64 UEFI loader and ELF64 kernel (entry=0x{entry:x})")
