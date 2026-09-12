#include "main.h"

/* Published state for later kernel subsystems and debugger inspection. */
const BOOT_INFO *kernel_boot_info;
volatile uint64_t kernel_initial_rsp;
volatile uint64_t kernel_observed_rsp;

_Noreturn void kernel_main(const BOOT_INFO *info)
{
    uint64_t rsp;
    __asm__ volatile ("movq %%rsp, %0" : "=r"(rsp));
    kernel_observed_rsp = rsp;
    if (!info || info->magic != BOOT_INFO_MAGIC || info->version != BOOT_INFO_VERSION
        || info->size != sizeof(*info) || info->flags != BOOT_SERVICES_EXITED
        || !info->memory_map || !info->memory_map_size
        || info->descriptor_size < sizeof(BOOT_MEMORY_DESCRIPTOR)
        || info->memory_map_size % info->descriptor_size != 0
        || info->descriptor_version != 1 || info->reserved != 0
        || info->stack_size < 4096 || info->stack_base > UINT64_MAX - info->stack_size
        || rsp < info->stack_base || rsp >= info->stack_base + info->stack_size
        || kernel_initial_rsp != info->stack_base + info->stack_size
        || (kernel_initial_rsp & 15) != 0) {
        /* Failure remains distinguishable from a successful HLT. */
        for (;;) __asm__ volatile ("pause");
    }
    kernel_boot_info = info;
    kernel_halt();
}
