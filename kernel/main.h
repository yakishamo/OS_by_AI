#ifndef KERNEL_MAIN_H
#define KERNEL_MAIN_H

#include "../include/boot_info.h"

_Noreturn void kernel_main(const BOOT_INFO *info);
_Noreturn void kernel_halt(void);

#endif
