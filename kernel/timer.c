#include "timer.h"
#include "irq.h"
#include "../include/x86.h"

static volatile uint64_t ticks;
static bool initialized;

bool timer_init(void)
{
    if (initialized || (x86_read_rflags() & 0x200)) return false;
    if (!irq_init()) return false;
    /* PIT channel0, low/high byte, binary mode2. 1193182/11932 ~=100 Hz. */
    x86_outb(0x43, 0x34);
    x86_outb(0x40, 11932 & 255);
    x86_outb(0x40, 11932 >> 8);
    ticks = 0;
    initialized = true;
    return irq_enable(0);
}

uint64_t timer_ticks(void)
{
    /* Aligned native-width load, sole writer is IRQ0 on this same CPU. */
    return ticks;
}

void timer_interrupt(void)
{
    ++ticks;
}
