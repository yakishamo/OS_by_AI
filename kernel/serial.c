#include "../include/x86.h"
#include "serial.h"
#include "irq.h"
#include "rx_buffer.h"
#include <stdint.h>

#define COM1 0x3f8
#define POLL_LIMIT 1000000u

static RX_BUFFER receive_buffer;
static bool receive_enabled;
volatile uint64_t serial_rx_interrupts, serial_rx_dropped;
volatile uint64_t serial_rx_overflows;

static void receive_error(void)
{
    uint64_t flags = x86_irq_save();
    rx_fail(&receive_buffer);
    ++serial_rx_dropped;
    x86_irq_restore(flags);
}

static bool wait_status(uint8_t mask)
{
    /* A bounded poll, not a time-based timeout (no kernel timer yet). */
    for (unsigned i = 0; i < POLL_LIMIT; ++i) {
        uint8_t status = x86_inb(COM1 + 5);
        if (status == 0xff) return false;
        /* Reading LSR clears errors even while waiting for transmit readiness. */
        if (receive_enabled && (status & 0x1e)) receive_error();
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
    return rx_read(&receive_buffer, byte);
}

bool serial_enable_receive(void)
{
    if (receive_enabled || (x86_read_rflags() & 0x200)) return false;
    if (!irq_enable(4)) return false;
    rx_reset(&receive_buffer);
    x86_outb(COM1 + 2, 0x07); /* Clear old FIFO contents; threshold one byte. */
    x86_outb(COM1 + 4, 0x0b); /* DTR/RTS + OUT2 interrupt gate. */
    receive_enabled = true;
    x86_outb(COM1 + 1, 0x05); /* Received data and line status; no TX interrupts. */
    return true;
}

static void receive_byte(void)
{
    uint8_t status = x86_inb(COM1 + 5);
    if (status & 0x1e) receive_error();
    if (!(status & 1)) return;
    uint8_t byte = x86_inb(COM1);
    bool already_failed = receive_buffer.failed;
    if (!rx_push(&receive_buffer, byte)) {
        if (!already_failed) ++serial_rx_overflows;
        ++serial_rx_dropped;
    }
}

void serial_interrupt(void)
{
    ++serial_rx_interrupts;
    /* Bound IRQ time under a continuous stream. Drain both data and timeout IRQs. */
    for (unsigned i = 0; i < RX_CAPACITY; ++i) {
        uint8_t cause = x86_inb(COM1 + 2);
        if (cause & 1) return;
        switch (cause & 0x0e) {
        case 0x04: /* Receive data available. */
        case 0x0c: /* Receive FIFO timeout. */
        case 0x06: /* Line status. */
            receive_byte();
            break;
        default:
            /* An unconfigured interrupt source is a device error. */
            x86_outb(COM1 + 1, 0);
            receive_error();
            return;
        }
    }
    receive_error();
    x86_outb(COM1 + 2, 0x03); /* Clear RX only; preserve pending transmit data. */
}
