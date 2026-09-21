#ifndef KERNEL_TIMER_H
#define KERNEL_TIMER_H
#include <stdbool.h>
#include <stdint.h>

/* QEMU q35, one CPU, legacy PIC/PIT. Call after IDT installation with IF=0.
 * Initialization leaves IF=0; the caller enables interrupts after setup.
 */
bool timer_init(void);
uint64_t timer_ticks(void);
void timer_interrupt(void);
/* Boot diagnostic: temporarily enable IRQs, verify GPRs and DF/CF survive. */
bool timer_check_registers(void);
#endif
