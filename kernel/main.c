#include "../include/x86.h"
#include "main.h"
#include "serial.h"
#include "tables.h"
#include "pmm.h"
#include "paging.h"
#include "console.h"
#include "timer.h"

extern bool pmm_boot_check(void);
extern bool paging_boot_check(void);

/* Published state for later kernel subsystems and debugger inspection. */
const BOOT_INFO *kernel_boot_info;
volatile uint64_t kernel_initial_rsp;
volatile uint64_t kernel_observed_rsp;

static bool valid_boot_header(const BOOT_INFO *info)
{
    if (!info || info->magic != BOOT_INFO_MAGIC) return false;
    if (info->version != BOOT_INFO_VERSION || info->size != sizeof(*info)) return false;
    return info->flags == BOOT_SERVICES_EXITED && info->reserved == 0;
}

static bool valid_memory_map(const BOOT_INFO *info)
{
    if (!info->memory_map || !info->memory_map_size) return false;
    if (info->descriptor_size < sizeof(BOOT_MEMORY_DESCRIPTOR)) return false;
    if (info->memory_map_size % info->descriptor_size != 0) return false;
    return info->descriptor_version == 1;
}

static bool valid_stack(const BOOT_INFO *info, uint64_t rsp)
{
    if (info->stack_size < 4096) return false;
    if (info->stack_base > UINT64_MAX - info->stack_size) return false;
    uint64_t top = info->stack_base + info->stack_size;
    if (rsp < info->stack_base || rsp >= top) return false;
    return kernel_initial_rsp == top && (kernel_initial_rsp & 15) == 0;
}

static void boot_log(const char *message)
{
    if (!serial_write(message) || !serial_flush()) x86_spin_forever();
}

static _Noreturn void boot_failure(const char *message)
{
    serial_write(message);
    serial_flush();
    x86_spin_forever();
}

static void initialize_memory(const BOOT_INFO *info)
{
    if (!pmm_init(info) || !pmm_boot_check())
        boot_failure("KERNEL ERROR: physical page management\n");
    boot_log("KERNEL: physical pages ready (4 KiB, self-test passed)\n");

    if (!paging_init(info) || !pmm_boot_check())
        boot_failure("KERNEL ERROR: paging initialization or RAM check\n");
    boot_log("KERNEL: paging ready (own CR3, RAM check passed)\n");

    if (!paging_boot_check()) boot_failure("KERNEL ERROR: dynamic paging check\n");
    boot_log("KERNEL: dynamic paging checks passed\n");
}

static void initialize_timer(void)
{
    if (!timer_init()) x86_spin_forever();
    if (!timer_check_registers() || !timer_ticks()) x86_spin_forever();
    boot_log("KERNEL: timer ready (PIT, ~100 Hz)\n");
}

_Noreturn void kernel_main(const BOOT_INFO *info)
{
    /* Capture the entry stack here, before calling validation helpers. */
    uint64_t rsp = x86_read_rsp();
    kernel_observed_rsp = rsp;
    if (!valid_boot_header(info)) x86_spin_forever();
    if (!valid_memory_map(info) || !valid_stack(info, rsp)) x86_spin_forever();
    kernel_boot_info = info;

    if (!serial_init()) x86_spin_forever();
    boot_log("KERNEL: serial ready (COM1, 115200 8N1)\n"
             "KERNEL: boot information verified\n");
    tables_init(info->stack_base + info->stack_size);
    boot_log("KERNEL: GDT/IDT/TSS ready\n");
    initialize_memory(info);
    initialize_timer();
    x86_enable_interrupts();
    console_run();
}
