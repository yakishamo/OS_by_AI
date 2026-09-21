"""UART test transport and burst/recovery checks, separate from boot validation."""
import os
import re
import time


class SerialClient:
    def __init__(self, connection, output_pipe, log, deadline):
        self.connection = connection
        self.output = os.open(output_pipe, os.O_RDWR | os.O_NONBLOCK)
        self.log = log
        self.deadline = deadline

    def close(self):
        os.close(self.output)

    def read(self):
        # Drain the output pipe so verbose commands cannot stall QEMU's backend.
        try:
            while os.read(self.output, 65536):
                pass
        except BlockingIOError:
            pass
        return self.log.read_bytes()

    def send(self, data, paced=True):
        if not paced:
            if self.connection.write(data) != len(data):
                raise RuntimeError("Incomplete serial burst")
            return
        for byte in data:
            self.connection.write(bytes([byte]))
            time.sleep(0.003)

    def command(self, data, expected, prompts=1, paced=True):
        start = len(self.read())
        self.send(data, paced)
        while time.monotonic() < self.deadline:
            reply = self.read()[start:]
            if reply.endswith(b"K> ") and reply.count(b"K> ") >= prompts:
                if expected not in reply or reply.count(b"K> ") != prompts:
                    raise RuntimeError(f"Console reply mismatch: {reply!r}")
                return reply
            time.sleep(0.01)
        raise RuntimeError("Console command timed out")

    def wait_quiet(self):
        last = self.read()
        since = time.monotonic()
        while time.monotonic() < self.deadline:
            current = self.read()
            if current != last:
                last, since = current, time.monotonic()
            if time.monotonic() - since > 0.3:
                return
            time.sleep(0.01)
        raise RuntimeError("Serial output did not settle")


def check_receive_bursts(client, monitor, symbols):
    reply = client.command(b"mem\r" * 16, b"pages: total=", prompts=16, paced=False)
    if reply.count(b"pages: total=") != 16:
        raise RuntimeError("Buffered commands lost or reordered")
    print("PASS: UART IRQ burst buffering", flush=True)


def check_receive_overflow(client, monitor, symbols):
    # The test-only link wrapper holds the consumer until the producer overflows.
    client.send(b"a" * 256, paced=False)
    while time.monotonic() < client.deadline:
        dump = monitor(f"xp /1gx 0x{symbols['serial_rx_overflows']:x}")
        value = re.search(r":\s*0x([0-9a-fA-F]+)", dump)
        if value and int(value[1], 16) > 0:
            break
        client.read()
        time.sleep(0.01)
    else:
        raise RuntimeError("Burst did not exercise RX buffer overflow")
    client.wait_quiet()
    client.command(b"\r", b"Input discarded (line too long or serial error).")
    client.command(b"help\r", b"help  - list commands")
    print("PASS: UART IRQ buffer overflow, line discard and console recovery", flush=True)
