#ifndef KERNEL_SERIAL_H
#define KERNEL_SERIAL_H

#include <stdbool.h>
#include <stdint.h>

/* QEMU PC COM1, polling I/O. No firmware or interrupt dependency. */
bool serial_init(void);
bool serial_write(const char *text);
bool serial_flush(void);
/* Nonblocking: 1 = byte received, 0 = no data, -1 = receive error. */
int serial_read(uint8_t *byte);

#endif
