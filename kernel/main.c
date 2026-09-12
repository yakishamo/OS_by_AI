#include "main.h"

_Noreturn void kernel_main(void)
{
    /* No interrupt handlers yet. Never return to the retired UEFI environment. */
    __asm__ volatile ("cli" : : : "memory");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
