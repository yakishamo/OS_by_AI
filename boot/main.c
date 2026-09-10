#include "serial.h"

/* The Windows x64 target supplies the UEFI x64 calling convention.
 * This environment probe does not use UEFI services or load a kernel yet.
 */
uintptr_t efi_main(void *image_handle, void *system_table)
{
    (void)image_handle;
    (void)system_table;

    serial_init();
    serial_write("\nBOOT: UEFI x86_64 C entry\n");
    serial_write("BOOT: serial ready (115200 8N1)\n");
    serial_write("BOOT: environment probe; kernel not loaded\n");
    serial_write("Type to echo; Ctrl-a x exits QEMU.\n");
    serial_write("SERIAL> ");

    for (;;) {
        int character = serial_read();
        if (character == '\r' || character == '\n') {
            serial_write("\nSERIAL> ");
        } else if (character >= 0) {
            serial_putc((char)character);
        } else {
            __asm__ volatile ("pause");
        }
    }
}
