#include "../include/x86.h"
#include "paging.h"
#include "layout.h"

extern const BOOT_INFO *kernel_boot_info;

static void check_failed(void)
{
    x86_spin_forever();
}

static bool rejects_invalid_mappings(uint64_t p)
{
    const uint64_t v = PAGING_DYNAMIC_BASE;
    uint64_t result = 123;
    unsigned flags = 123;
    /* Kernel and guard frames cannot be aliased through the dynamic API. */
    if (paging_map(v, (uintptr_t)kernel_text_start, PAGING_WRITE)) return false;
    if (paging_map(v, (uintptr_t)kernel_rodata_start, PAGING_WRITE)) return false;
    if (paging_map(v, kernel_boot_info->stack_base - 4096, PAGING_WRITE)) return false;
    if (paging_map(v, (uintptr_t)double_fault_guard_low, PAGING_WRITE)) return false;
    if (paging_map(0x100000, p, 0)) return false;
    if (paging_map(v + 1, p, 0)) return false;
    if (paging_map(v + PAGING_DYNAMIC_SIZE, p, 0)) return false;
    if (paging_map(UINT64_C(0x800000000000), p, 0)) return false;
    if (paging_map(v, p + 1, 0)) return false;
    if (paging_map(v, 0, 0)) return false;
    if (paging_map(v, p, 4)) return false;
    if (paging_unmap(v)) return false;
    if (paging_protect(v, 0)) return false;
    if (paging_query(v, &result, &flags)) return false;
    if (result != 123) return false;
    if (flags != 123) return false;
    return true;
}

static bool check_aliases_and_permissions(void)
{
    uint64_t before = pmm_available(), p, q, result;
    unsigned flags;
    const uint64_t v = PAGING_DYNAMIC_BASE, other = v + 0x200000;
    if (!pmm_alloc(&p) || !pmm_alloc(&q)) return false;
    if (!rejects_invalid_mappings(p)) return false;
    if (!paging_map(v, p, PAGING_WRITE)) return false;
    if (paging_map(v, q, 0)) return false;
    if (!paging_map(other, p, 0)) return false;
    volatile uint64_t *alias = (void *)(uintptr_t)v;
    *alias = UINT64_C(0x123456789abcdef0);
    if (*(volatile uint64_t *)(uintptr_t)p != *alias
        || *(volatile uint64_t *)(uintptr_t)other != *alias) return false;
    if (!paging_query(v, &result, &flags)) return false;
    if (result != p) return false;
    if (flags != PAGING_WRITE) return false;
    if (!paging_protect(v, 0)) return false;
    if (!paging_query(v, &result, &flags)) return false;
    if (flags != 0) return false;
    if (!paging_protect(v, PAGING_WRITE)) return false;
    *alias = 42; /* A warmed translation must become writable again. */
    if (!paging_unmap(v)) return false;
    if (paging_query(v, &result, &flags)) return false;
    if (paging_unmap(v)) return false;
    if (!paging_map(v, q, PAGING_WRITE)) return false;
    *alias = 99; /* Remapping must discard the previous physical translation. */
    if (*(volatile uint64_t *)(uintptr_t)q != 99 || *(volatile uint64_t *)(uintptr_t)p != 42)
        return false;
    if (!paging_unmap(v)) return false;
    if (!paging_unmap(other)) return false;
    if (!pmm_free(p)) return false;
    if (!pmm_free(q)) return false;
    if (pmm_available() != before) return false;

    return true;
}

static bool check_failed_map_cleanup(void)
{
    uint64_t before = pmm_available(), p, q, result;
    unsigned flags;
    const uint64_t v = PAGING_DYNAMIC_BASE;
    /* Leave only two free pages: creating a fresh dynamic path needs three.
     * Link held pages through their identity mapping, without a large array.
     */
    uint64_t held = 0;
    if (!pmm_alloc(&p)) return false;
    while (pmm_available() > 2) {
        if (!pmm_alloc(&q)) return false;
        *(uint64_t *)(uintptr_t)q = held;
        held = q;
    }
    if (paging_map(v, p, PAGING_WRITE)) return false;
    if (pmm_available() != 2) return false;
    if (paging_query(v, &result, &flags)) return false;
    while (held) {
        q = *(uint64_t *)(uintptr_t)held;
        if (!pmm_free(held)) return false;
        held = q;
    }
    if (!paging_map(v, p, PAGING_WRITE)) return false;
    if (!paging_unmap(v)) return false;
    if (!pmm_free(p)) return false;
    return pmm_available() == before;
}

bool paging_boot_check(void)
{
    if (!check_aliases_and_permissions()) return false;
    return check_failed_map_cleanup();
}

/* QEMU fixtures jump here instead of kernel_halt. Never called in normal boot. */
static void fault_check(unsigned mode)
{
    uint64_t p;
    const uint64_t v = PAGING_DYNAMIC_BASE;
    if (!pmm_alloc(&p) || !paging_map(v, p, PAGING_WRITE)) check_failed();
    volatile uint64_t *alias = (void *)(uintptr_t)v;
    *alias = 0xc3; /* ret instruction, also warm the writable TLB entry. */
    if (mode == 0) {
        if (!paging_protect(v, 0)) check_failed();
        *alias = 1;
    } else if (mode == 1) {
        if (!paging_protect(v, PAGING_EXEC)) check_failed();
        ((void (*)(void))(uintptr_t)v)(); /* Verify execution before revoking it. */
        if (!paging_protect(v, 0)) check_failed();
        ((void (*)(void))(uintptr_t)v)();
    } else {
        if (!paging_unmap(v)) check_failed();
        (void)*alias;
    }
    check_failed();
}
void paging_test_readonly(void) { fault_check(0); }
void paging_test_nx(void) { fault_check(1); }
void paging_test_unmapped(void) { fault_check(2); }

void paging_test_text_write(void) { *(volatile char *)kernel_text_start = 0; check_failed(); }
void paging_test_rodata_write(void) { *(volatile char *)kernel_rodata_start = 0; check_failed(); }
static volatile unsigned char nx_data = 0xc3;
void paging_test_data_exec(void) { ((void (*)(void))(uintptr_t)&nx_data)(); check_failed(); }
void paging_test_stack_exec(void)
{
    volatile unsigned char code = 0xc3;
    ((void (*)(void))(uintptr_t)&code)();
    check_failed();
}
void paging_test_stack_low(void)
{ (void)*(volatile char *)(uintptr_t)(kernel_boot_info->stack_base - 4096); check_failed(); }
void paging_test_stack_high(void)
{ (void)*(volatile char *)(uintptr_t)(kernel_boot_info->stack_base + kernel_boot_info->stack_size); check_failed(); }
void paging_test_df_low(void) { (void)*(volatile char *)double_fault_guard_low; check_failed(); }
void paging_test_df_high(void) { (void)*(volatile char *)double_fault_guard_high; check_failed(); }
