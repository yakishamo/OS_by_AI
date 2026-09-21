#ifndef KERNEL_SERIAL_H
#define KERNEL_SERIAL_H

#include <stdbool.h>
#include <stdint.h>

/* QEMU PC COM1, polling output and interrupt-driven input. */
bool serial_init(void);
bool serial_write(const char *text);
bool serial_flush(void);
/* Both require IF=0. Enable only after PIC initialization.
 * Read: 1 = byte, 0 = empty, -1 = input lost (discard the current line).
 */
bool serial_enable_receive(void);
int serial_read(uint8_t *byte);
void serial_interrupt(void);

#endif
