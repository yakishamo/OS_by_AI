#include "pmm.h"

#define PAGE_COUNT (PMM_LIMIT / PMM_PAGE_SIZE)
#define WORD_COUNT (PAGE_COUNT / 64)
static uint64_t eligible[WORD_COUNT];
static uint64_t allocated[WORD_COUNT];
static uint64_t total_pages, free_pages, cursor;
static bool ready;

static bool bit(const uint64_t *map, uint64_t page)
{
    return (map[page / 64] & (UINT64_C(1) << (page % 64))) != 0;
}
static void set(uint64_t *map, uint64_t page)
{
    map[page / 64] |= UINT64_C(1) << (page % 64);
}
static void clear(uint64_t *map, uint64_t page)
{
    map[page / 64] &= ~(UINT64_C(1) << (page % 64));
}
static bool valid_range(uint64_t base, uint64_t size)
{
    return size != 0 && base <= UINT64_MAX - size;
}
static void reserve(uint64_t base, uint64_t size)
{
    if (base >= PMM_LIMIT) return;
    uint64_t end = base + size;
    if (end > PMM_LIMIT) end = PMM_LIMIT;
    for (uint64_t page = base / PMM_PAGE_SIZE; page < (end + 4095) / 4096; ++page)
        clear(eligible, page);
}
static const BOOT_MEMORY_DESCRIPTOR *descriptor(const BOOT_INFO *info, uint64_t index)
{
    return (const void *)(uintptr_t)(info->memory_map + index * info->descriptor_size);
}

bool pmm_init(const BOOT_INFO *info)
{
    if (ready || !info || info->magic != BOOT_INFO_MAGIC || info->version != BOOT_INFO_VERSION
        || info->size != sizeof(*info) || info->flags != BOOT_SERVICES_EXITED
        || !info->memory_map || info->memory_map % 8 || info->descriptor_size < 40
        || info->descriptor_size % 8 || info->descriptor_version != 1
        || !valid_range(info->memory_map, info->memory_map_size) || info->memory_map_size > 65536
        || info->memory_map_size % info->descriptor_size
        || !valid_range(info->kernel_base, info->kernel_size)
        || !valid_range(info->stack_base, info->stack_size)
        || !valid_range((uintptr_t)info, sizeof(*info))) return false;
    uint64_t count = info->memory_map_size / info->descriptor_size;
    /* Validate the complete map before changing allocator state. Reject overlaps
     * rather than depending on descriptor ordering to resolve conflicting types.
     */
    for (uint64_t i = 0; i < count; ++i) {
        const BOOT_MEMORY_DESCRIPTOR *d = descriptor(info, i);
        if (d->physical_start % 4096 || !d->number_of_pages
            || d->number_of_pages > (UINT64_MAX - d->physical_start) / 4096) return false;
        uint64_t end = d->physical_start + d->number_of_pages * 4096;
        for (uint64_t j = 0; j < i; ++j) {
            const BOOT_MEMORY_DESCRIPTOR *other = descriptor(info, j);
            if (d->physical_start < other->physical_start + other->number_of_pages * 4096
                && other->physical_start < end) return false;
        }
    }
    /* Both bitmaps start zero in BSS and initialization cannot be repeated. */
    for (uint64_t i = 0; i < count; ++i) {
        const BOOT_MEMORY_DESCRIPTOR *d = descriptor(info, i);
        if (d->type != 7 || (d->attributes & (UINT64_C(1) << 63))) continue;
        uint64_t begin = d->physical_start, end = begin + d->number_of_pages * 4096;
        if (begin < 0x100000) begin = 0x100000;
        if (end > PMM_LIMIT) end = PMM_LIMIT;
        for (uint64_t page = begin / 4096; page < end / 4096; ++page) set(eligible, page);
    }
    reserve(info->kernel_base, info->kernel_size); /* Includes this allocator's BSS. */
    reserve((uintptr_t)info, sizeof(*info));
    reserve(info->memory_map, info->memory_map_size);
    reserve(info->stack_base, info->stack_size);
    for (uint64_t page = 0; page < PAGE_COUNT; ++page)
        if (bit(eligible, page)) ++total_pages;
    free_pages = total_pages;
    ready = true; /* Zero usable pages is valid; allocations will fail. */
    return true;
}

bool pmm_alloc(uint64_t *address)
{
    if (!ready || !address || !free_pages) return false;
    for (uint64_t scanned = 0; scanned < PAGE_COUNT; ++scanned) {
        uint64_t page = (cursor + scanned) % PAGE_COUNT;
        if (!bit(eligible, page) || bit(allocated, page)) continue;
        set(allocated, page);
        --free_pages;
        cursor = (page + 1) % PAGE_COUNT;
        *address = page * 4096;
        return true;
    }
    return false;
}

bool pmm_free(uint64_t address)
{
    if (!ready || address % 4096 || address >= PMM_LIMIT) return false;
    uint64_t page = address / 4096;
    if (!bit(eligible, page) || !bit(allocated, page)) return false;
    clear(allocated, page);
    ++free_pages;
    if (page < cursor) cursor = page;
    return true;
}

uint64_t pmm_total(void) { return total_pages; }
uint64_t pmm_available(void) { return free_pages; }
bool pmm_is_allocated(uint64_t address)
{
    return ready && address < PMM_LIMIT && address % 4096 == 0
        && bit(eligible, address / 4096) && bit(allocated, address / 4096);
}
