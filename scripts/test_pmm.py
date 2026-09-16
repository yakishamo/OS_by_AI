"""Exercise the actual allocator with synthetic maps, without accessing RAM."""
import ctypes as c
from pathlib import Path
import struct


class Info(c.Structure):
    _fields_ = [(name, kind) for name, kind in (
        ("magic", c.c_uint64), ("version", c.c_uint32), ("size", c.c_uint32),
        ("memory_map", c.c_uint64), ("memory_map_size", c.c_uint64),
        ("descriptor_size", c.c_uint64), ("descriptor_version", c.c_uint32),
        ("reserved", c.c_uint32), ("kernel_base", c.c_uint64),
        ("kernel_size", c.c_uint64), ("stack_base", c.c_uint64),
        ("stack_size", c.c_uint64), ("flags", c.c_uint64))]


lib = c.CDLL(str(Path(__file__).resolve().parents[1] / "build/pmm-test.so"))
lib.pmm_init.argtypes, lib.pmm_init.restype = [c.POINTER(Info)], c.c_bool
lib.pmm_alloc.argtypes, lib.pmm_alloc.restype = [c.POINTER(c.c_uint64)], c.c_bool
lib.pmm_free.argtypes, lib.pmm_free.restype = [c.c_uint64], c.c_bool
for name in ("pmm_total", "pmm_available"):
    getattr(lib, name).argtypes = []
    getattr(lib, name).restype = c.c_uint64


def memory_map(entries):
    # Deliberately use a 48-byte stride, including a nonzero extension.
    return c.create_string_buffer(b"".join(
        struct.pack("<IIQQQQQ", kind, 0, base, 0, pages, attributes, 0xdeadbeef)
        for kind, base, pages, attributes in entries))


info = Info(0x4f5342494e464f31, 1, 88, 0, 0, 48, 1, 0,
            0x102001, 4096, 0x106000, 4096, 1)
address = c.c_uint64(123)
assert not lib.pmm_alloc(c.byref(address)) and address.value == 123
assert not lib.pmm_free(0x100000)
assert not lib.pmm_init(None)

# Bad descriptors must fail before changing allocator state.
for entries in (
    [(7, 0x100001, 1, 0)],
    [(7, 0x100000, 0, 0)],
    [(7, 0xfffffffffffff000, 2, 0)],
    [(7, 0x100000, 3, 0), (4, 0x101000, 1, 0)],
):
    buffer = memory_map(entries)
    info.memory_map, info.memory_map_size = c.addressof(buffer), len(entries) * 48
    assert not lib.pmm_init(c.byref(info))
    assert lib.pmm_total() == 0

entries = [(7, 0xfffff000, 3, 0), (4, 0x108000, 2, 0),
           (7, 0xff000, 9, 0), (7, 0x200000, 2, 1 << 63)]
buffer = memory_map(entries)
info.memory_map, info.memory_map_size = c.addressof(buffer), len(entries) * 48
for field, invalid in (("descriptor_size", 39), ("descriptor_version", 2),
                       ("memory_map_size", 191), ("flags", 0), ("version", 2)):
    old = getattr(info, field)
    setattr(info, field, invalid)
    assert not lib.pmm_init(c.byref(info))
    setattr(info, field, old)
assert lib.pmm_init(c.byref(info))
assert not lib.pmm_init(c.byref(info))
expected = {0x100000, 0x101000, 0x104000, 0x105000, 0x107000, 0xfffff000}
assert lib.pmm_total() == lib.pmm_available() == len(expected)
assert not lib.pmm_alloc(None)
actual = set()
for _ in expected:
    assert lib.pmm_alloc(c.byref(address))
    assert address.value not in actual
    actual.add(address.value)
assert actual == expected and lib.pmm_available() == 0
address.value = 123
assert not lib.pmm_alloc(c.byref(address)) and address.value == 123
for invalid in (0, 0x102000, 0x106000, 0x108000, 0x200000, 0x100001, 0x100000000):
    assert not lib.pmm_free(invalid)
for page in sorted(actual):
    assert lib.pmm_free(page)
    assert not lib.pmm_free(page)
assert lib.pmm_available() == len(expected)
recycled = set()
for _ in expected:
    assert lib.pmm_alloc(c.byref(address))
    recycled.add(address.value)
assert recycled == expected
print("PASS: physical pages: map validation, reservations, limits, exhaustion and reuse")
