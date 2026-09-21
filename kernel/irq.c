#include "irq.h"
#include "timer.h"
#include "serial.h"
#include "../include/x86.h"

static bool initialized;
volatile uint64_t unexpected_irqs;

static void pic_write(uint16_t port, uint8_t value)
{
    x86_outb(port, value);
    x86_outb(0x80, 0); /* Legacy I/O delay. */
}

bool irq_init(void)
{
    if (initialized || (x86_read_rflags() & 0x200)) return false;
    /* Use direct legacy PIC delivery instead of inherited local APIC state.
     * x2APIC and SMP are outside this initial platform contract.
     */
    if (x86_cpuid(1, 0).edx & (1u << 9)) {
        uint64_t apic = x86_read_msr(0x1b);
        if (apic & (UINT64_C(1) << 10)) return false;
        x86_write_msr(0x1b, apic & ~(UINT64_C(1) << 11));
    }
    pic_write(0x21, 0xff);
    pic_write(0xa1, 0xff);
    pic_write(0x20, 0x11); pic_write(0xa0, 0x11); /* ICW1: cascaded, ICW4 follows. */
    pic_write(0x21, 0x20); pic_write(0xa1, 0x28); /* IRQ0..15 -> vectors32..47. */
    pic_write(0x21, 4); pic_write(0xa1, 2);       /* Slave on master IRQ2. */
    pic_write(0x21, 1); pic_write(0xa1, 1);       /* 8086 mode, explicit EOI. */
    pic_write(0x21, 0xff); pic_write(0xa1, 0xff);
    initialized = true;
    return true;
}

bool irq_enable(unsigned irq)
{
    if (!initialized || irq >= 16 || (x86_read_rflags() & 0x200)) return false;
    uint16_t port = irq < 8 ? 0x21 : 0xa1;
    pic_write(port, x86_inb(port) & ~(1u << (irq & 7)));
    if (irq >= 8) pic_write(0x21, x86_inb(0x21) & ~4u);
    return true;
}

void irq_dispatch(uint64_t vector)
{
    if (vector == 39 || vector == 47) {
        uint16_t port = vector == 39 ? 0x20 : 0xa0;
        x86_outb(port, 0x0b); /* Read in-service register, detect spurious IRQ7/15. */
        if (!(x86_inb(port) & 0x80)) {
            if (vector == 47) x86_outb(0x20, 0x20);
            return;
        }
    }
    if (vector == 32) timer_interrupt();
    else if (vector == 36) serial_interrupt();
    else {
        ++unexpected_irqs;
        uint16_t mask_port = vector < 40 ? 0x21 : 0xa1;
        x86_outb(mask_port, x86_inb(mask_port) | (1u << ((vector - 32) & 7)));
    }
    if (vector >= 40) x86_outb(0xa0, 0x20);
    x86_outb(0x20, 0x20);
}
