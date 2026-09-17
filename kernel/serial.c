#include "../include/x86.h"
#include "serial.h"
#include <stdint.h>

#define COM1 0x3f8
#define POLL_LIMIT 1000000u

static bool wait_status(uint8_t mask)
{
    /* A bounded poll, not a time-based timeout (no kernel timer yet). */
    for (unsigned i = 0; i < POLL_LIMIT; ++i) {
        uint8_t status = x86_inb(COM1 + 5);
        if (status == 0xff) return false;
        if ((status & mask) == mask) return true;
        x86_pause();
    }
    return false;
}

bool serial_init(void)
{
    /* Let the loader's last byte leave before reconfiguring the same UART. */
    if (!serial_flush()) return false;
    x86_outb(COM1 + 3, 0x03); /* DLAB off. */
    x86_outb(COM1 + 1, 0x00); /* UART interrupts disabled. */
    x86_outb(COM1 + 3, 0x80); /* Divisor latch access. */
    x86_outb(COM1 + 0, 0x01); /* 115200 baud divisor. */
    x86_outb(COM1 + 1, 0x00);
    x86_outb(COM1 + 3, 0x03); /* 8 data bits, no parity, 1 stop bit. */
    x86_outb(COM1 + 2, 0x07); /* Enable and clear FIFOs. */
    x86_outb(COM1 + 4, 0x03); /* DTR/RTS, loopback off. */
    return wait_status(0x20);
}

static bool put_byte(uint8_t value)
{
    if (!wait_status(0x20)) return false; /* Transmit holding register empty. */
    x86_outb(COM1, value);
    return true;
}

bool serial_write(const char *text)
{
    for (; *text; ++text) {
        if (*text == '\n' && !put_byte('\r')) return false;
        if (!put_byte((uint8_t)*text)) return false;
    }
    return true;
}

bool serial_flush(void)
{
    return wait_status(0x40); /* Both FIFO and shift register are empty. */
}

int serial_read(uint8_t *byte)
{
    uint8_t status = x86_inb(COM1 + 5);
    if (status == 0xff) return -1;
    if (status & 0x1e) { /* Overrun, parity, framing or break. Discard bad byte. */
        if (status & 1) (void)x86_inb(COM1);
        return -1;
    }
    if (!(status & 1)) return 0;
    *byte = x86_inb(COM1);
    return 1;
}
