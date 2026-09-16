#ifndef KERNEL_PAGING_H
#define KERNEL_PAGING_H
#include "pmm.h"

/* Boot-only, after pmm_init, on one CPU with interrupts disabled.
 * Build and activate an identity map. Failure leaves the old CR3 active.
 */
bool paging_init(const BOOT_INFO *info);

#define PAGING_DYNAMIC_BASE UINT64_C(0xffff800000000000)
#define PAGING_DYNAMIC_SIZE (UINT64_C(1) << 39)
#define PAGING_WRITE 1u
#define PAGING_EXEC 2u
/* 4 KiB pages, supervisor only, single CPU with interrupts disabled.
 * Only the dynamic window can be changed. Physical pages must be allocated
 * by the caller and kept allocated until all aliases are unmapped.
 * map never replaces an existing mapping; unmap does not free the data page.
 * Zero flags means read-only and non-executable. Query outputs survive failure.
 */
bool paging_map(uint64_t virtual_address, uint64_t physical_address, unsigned flags);
bool paging_unmap(uint64_t virtual_address);
bool paging_protect(uint64_t virtual_address, unsigned flags);
bool paging_query(uint64_t virtual_address, uint64_t *physical_address, unsigned *flags);
#endif
