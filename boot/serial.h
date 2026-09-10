#ifndef BOOT_SERIAL_H
#define BOOT_SERIAL_H

#include <stdint.h>

void serial_init(void);
void serial_putc(char character);
void serial_write(const char *text);
int serial_read(void);

#endif
