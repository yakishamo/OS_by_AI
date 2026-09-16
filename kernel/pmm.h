#ifndef KERNEL_PMM_H
#define KERNEL_PMM_H
#include "../include/boot_info.h"
#include <stdbool.h>

#define PMM_PAGE_SIZE UINT64_C(4096)
#define PMM_LIMIT UINT64_C(0x100000000)

/* Single CPU, interrupts disabled. Initialization succeeds only once.
 * Addresses are physical; allocating a page does not create a mapping or clear it.
 */
bool pmm_init(const BOOT_INFO *info);
bool pmm_alloc(uint64_t *address);
bool pmm_free(uint64_t address);
uint64_t pmm_total(void);
uint64_t pmm_available(void);
#endif
