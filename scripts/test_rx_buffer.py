"""Exercise the same receive queue used by the UART interrupt handler."""
import ctypes as c
from pathlib import Path


class Buffer(c.Structure):
    _fields_ = [("data", c.c_uint8 * 256), ("head", c.c_uint),
                ("tail", c.c_uint), ("failed", c.c_bool)]


lib = c.CDLL(str(Path(__file__).resolve().parents[1] / "build/rx-test.so"))
for name in ("rx_reset", "rx_fail"):
    getattr(lib, name).argtypes = [c.POINTER(Buffer)]
    getattr(lib, name).restype = None
lib.rx_push.argtypes = [c.POINTER(Buffer), c.c_uint8]
lib.rx_push.restype = c.c_bool
lib.rx_read.argtypes = [c.POINTER(Buffer), c.POINTER(c.c_uint8)]
lib.rx_read.restype = c.c_int
buffer, byte = Buffer(), c.c_uint8(99)
lib.rx_reset(c.byref(buffer))


def push(value):
    return lib.rx_push(c.byref(buffer), value)


def read():
    return lib.rx_read(c.byref(buffer), c.byref(byte))


assert read() == 0 and byte.value == 99
for cycle in range(8):
    for value in range(255):
        assert push((cycle + value) % 256)
    for value in range(255):
        assert read() == 1 and byte.value == (cycle + value) % 256
    assert read() == 0

for value in range(255):
    assert push(value)
assert not push(255)  # Overflow invalidates the entire queued stream.
assert not push(13)
byte.value = 99
assert read() == -1 and byte.value == 99
assert read() == 0
assert push(13) and read() == 1 and byte.value == 13
assert push(42)
lib.rx_fail(c.byref(buffer))  # A UART line error has the same loss contract.
assert read() == -1 and read() == 0
assert push(7) and read() == 1 and byte.value == 7
print("PASS: RX buffer ordering, wraparound, capacity, loss reporting and recovery")
