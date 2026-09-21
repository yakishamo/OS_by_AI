#ifndef KERNEL_IRQ_H
#define KERNEL_IRQ_H
#include <stdbool.h>
#include <stdint.h>
/* Legacy PIC on QEMU q35 / one CPU. Configuration requires IF=0. */
bool irq_init(void);
bool irq_enable(unsigned irq);
void irq_dispatch(uint64_t vector);
#endif
