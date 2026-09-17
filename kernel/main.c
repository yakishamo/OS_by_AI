#include "../include/x86.h"
#include "main.h"
#include "serial.h"
#include "tables.h"
#include "pmm.h"
#include "paging.h"

extern bool pmm_boot_check(void);
extern bool paging_boot_check(void);

/* Published state for later kernel subsystems and debugger inspection. */
const BOOT_INFO *kernel_boot_info;
volatile uint64_t kernel_initial_rsp;
volatile uint64_t kernel_observed_rsp;

_Noreturn void kernel_main(const BOOT_INFO *info)
{
    uint64_t rsp = x86_read_rsp();
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
        x86_spin_forever();
    }
    kernel_boot_info = info;
    if (!serial_init()
        || !serial_write("KERNEL: serial ready (COM1, 115200 8N1)\n"
                         "KERNEL: boot information verified\n")
        || !serial_flush()) {
        /* Failed output must not be mistaken for a successful boot. */
        x86_spin_forever();
    }
    tables_init(info->stack_base + info->stack_size);
    if (!serial_write("KERNEL: GDT/IDT/TSS ready\n") || !serial_flush()) {
        x86_spin_forever();
    }
    if (!pmm_init(info) || !pmm_boot_check()) {
        serial_write("KERNEL ERROR: physical page management\n");
        serial_flush();
        x86_spin_forever();
    }
    if (!serial_write("KERNEL: physical pages ready (4 KiB, self-test passed)\n") || !serial_flush()) {
        x86_spin_forever();
    }
    if (!paging_init(info) || !pmm_boot_check()) {
        serial_write("KERNEL ERROR: paging initialization or RAM check\n");
        serial_flush();
        x86_spin_forever();
    }
    if (!serial_write("KERNEL: paging ready (own CR3, RAM check passed)\n") || !serial_flush()) {
        x86_spin_forever();
    }
    if (!paging_boot_check()) {
        serial_write("KERNEL ERROR: dynamic paging check\n");
        serial_flush();
        x86_spin_forever();
    }
    if (!serial_write("KERNEL: dynamic paging checks passed\nKERNEL: halting\n") || !serial_flush()) {
        x86_spin_forever();
    }
    kernel_halt();
}
