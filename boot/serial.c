#include "serial.h"

#define COM1 0x3f8

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void serial_init(void)
{
    outb(COM1 + 3, 0x03); /* Clear DLAB before accessing the interrupt register. */
    outb(COM1 + 1, 0x00); /* Polling; UART interrupts disabled. */
    outb(COM1 + 3, 0x80); /* Divisor latch access. */
    outb(COM1 + 0, 0x01); /* 115200 baud. */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); /* 8 data bits, no parity, 1 stop bit. */
    outb(COM1 + 2, 0xc7); /* Enable and clear FIFO. */
    outb(COM1 + 4, 0x03); /* DTR and RTS. */
}

void serial_putc(char character)
{
    while ((inb(COM1 + 5) & 0x20) == 0) {
        __asm__ volatile ("pause");
    }
    outb(COM1, (uint8_t)character);
}

void serial_write(const char *text)
{
    for (; *text != '\0'; ++text) {
        if (*text == '\n') {
            serial_putc('\r');
        }
        serial_putc(*text);
    }
}

int serial_read(void)
{
    if ((inb(COM1 + 5) & 0x01) == 0) {
        return -1;
    }
    return inb(COM1);
}
