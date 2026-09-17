#include "pmm.h"

/* Small real-RAM check during bring-up; leave every page free on success. */
bool pmm_boot_check(void)
{
    uint64_t before = pmm_available(), pages[3], again;
    if (before < 3) return false;
    for (unsigned i = 0; i < 3; ++i) {
        if (!pmm_alloc(&pages[i])) return false;
        for (unsigned j = 0; j < i; ++j) if (pages[i] == pages[j]) return false;
        volatile uint64_t *memory = (void *)(uintptr_t)pages[i];
        for (unsigned j = 0; j < 512; ++j) memory[j] = pages[i] ^ j;
    }
    for (unsigned i = 0; i < 3; ++i) {
        volatile uint64_t *memory = (void *)(uintptr_t)pages[i];
        for (unsigned j = 0; j < 512; ++j) if (memory[j] != (pages[i] ^ j)) return false;
    }
    if (pmm_available() != before - 3) return false;
    if (pmm_free(pages[0] + 1) || pmm_free(0)) return false;
    if (!pmm_free(pages[1]) || pmm_free(pages[1])) return false;
    if (!pmm_alloc(&again) || again != pages[1]) return false;
    if (!pmm_free(pages[0])) return false;
    if (!pmm_free(again) || !pmm_free(pages[2])) return false;
    return pmm_available() == before;
}
