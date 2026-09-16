#ifndef KERNEL_PAGING_H
#define KERNEL_PAGING_H
#include "pmm.h"

/* Boot-only, after pmm_init, on one CPU with interrupts disabled.
 * Build and activate an identity map. Failure leaves the old CR3 active.
 */
bool paging_init(const BOOT_INFO *info);
#endif
