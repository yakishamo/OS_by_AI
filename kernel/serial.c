#include "serial.h"
#include <stdint.h>

#define COM1 0x3f8
#define POLL_LIMIT 1000000u

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static bool wait_status(uint8_t mask)
{
    /* A bounded poll, not a time-based timeout (no kernel timer yet). */
    for (unsigned i = 0; i < POLL_LIMIT; ++i) {
        uint8_t status = inb(COM1 + 5);
        if (status == 0xff) return false;
        if ((status & mask) == mask) return true;
        __asm__ volatile ("pause");
    }
    return false;
}

bool serial_init(void)
{
    /* Let the loader's last byte leave before reconfiguring the same UART. */
    if (!serial_flush()) return false;
    outb(COM1 + 3, 0x03); /* DLAB off. */
    outb(COM1 + 1, 0x00); /* UART interrupts disabled. */
    outb(COM1 + 3, 0x80); /* Divisor latch access. */
    outb(COM1 + 0, 0x01); /* 115200 baud divisor. */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); /* 8 data bits, no parity, 1 stop bit. */
    outb(COM1 + 2, 0x07); /* Enable and clear FIFOs. */
    outb(COM1 + 4, 0x03); /* DTR/RTS, loopback off. */
    return wait_status(0x20);
}

static bool put_byte(uint8_t value)
{
    if (!wait_status(0x20)) return false; /* Transmit holding register empty. */
    outb(COM1, value);
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
