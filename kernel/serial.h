#ifndef KERNEL_SERIAL_H
#define KERNEL_SERIAL_H

#include <stdbool.h>

/* QEMU PC COM1, polling output only. No firmware or interrupt dependency. */
bool serial_init(void);
bool serial_write(const char *text);
bool serial_flush(void);

#endif
