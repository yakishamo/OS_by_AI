#include "paging.h"
#include "layout.h"
#include "../include/x86.h"

#define ADDRESS_MASK UINT64_C(0x000ffffffffff000)
#define FLAGS UINT64_C(3) /* Present, writable, supervisor; PAT index 0. */
#define MAX_TABLES 4096 /* Bounded table budget, including dynamic mappings. */
#define NX (UINT64_C(1) << 63)
static uint64_t table_pages[MAX_TABLES];
uint64_t paging_table_count;
uint64_t paging_root;
uint64_t paging_previous_cr3;
static bool active;

static uint64_t new_table(void)
{
    uint64_t address;
    if (paging_table_count == MAX_TABLES || !pmm_alloc(&address)) return 0;
    table_pages[paging_table_count++] = address;
    uint64_t *table = (void *)(uintptr_t)address;
    for (unsigned i = 0; i < 512; ++i) table[i] = 0;
    return address;
}

static bool map_page(uint64_t address)
{
    uint64_t *table = (void *)(uintptr_t)paging_root;
    for (unsigned shift = 39; shift > 12; shift -= 9) {
        unsigned index = (address >> shift) & 511;
        if (!(table[index] & 1)) {
            uint64_t child = new_table();
            if (!child) return false;
            table[index] = child | FLAGS;
        }
        table = (void *)(uintptr_t)(table[index] & ADDRESS_MASK);
    }
    uint64_t flags = FLAGS | NX;
    if (address >= (uintptr_t)kernel_text_start && address < (uintptr_t)kernel_text_end)
        flags = 1; /* Read/execute, never writable. */
    else if (address >= (uintptr_t)kernel_rodata_start && address < (uintptr_t)kernel_rodata_end)
        flags = 1 | NX;
    table[(address >> 12) & 511] = address | flags;
    return true;
}

static bool mapped_range(uint64_t base, uint64_t size)
{
    if (!size || base < 0x100000 || base >= PMM_LIMIT || size > PMM_LIMIT - base)
        return false;
    for (uint64_t page = base & ~UINT64_C(4095); page < base + size; page += 4096) {
        uint64_t *table = (void *)(uintptr_t)paging_root;
        for (unsigned shift = 39; ; shift -= 9) {
            uint64_t entry = table[(page >> shift) & 511];
            if (!(entry & 1)) return false;
            if (shift == 12) {
                if ((entry & ADDRESS_MASK) != page) return false;
                break;
            }
            table = (void *)(uintptr_t)(entry & ADDRESS_MASK);
        }
    }
    return true;
}

bool paging_init(const BOOT_INFO *info)
{
    if (active || !info || !pmm_total()) return false;
    uint64_t cr4 = x86_read_cr4(), rflags = x86_read_rflags();
    /* This first implementation supports four levels without PCID. */
    if ((cr4 & ((UINT64_C(1) << 12) | (UINT64_C(1) << 17))) || (rflags & 0x200))
        return false;
    if ((x86_read_msr(0x277) & 255) != 6) return false; /* PAT[0] must be write-back. */
    if (x86_cpuid(0x80000000, 0).eax < 0x80000001) return false;
    if (!(x86_cpuid(0x80000001, 0).edx & (1u << 20))) return false;
    /* The map has already been validated by pmm_init and remains immutable. */
    paging_root = new_table();
    if (!paging_root) return false;
    for (uint64_t offset = 0; offset < info->memory_map_size; offset += info->descriptor_size) {
        const BOOT_MEMORY_DESCRIPTOR *d = (const void *)(uintptr_t)(info->memory_map + offset);
        if ((d->type != 1 && d->type != 2 && d->type != 7)
            || (d->attributes & (UINT64_C(1) << 63))) continue;
        uint64_t begin = d->physical_start, end = begin + d->number_of_pages * 4096;
        if (begin < 0x100000) begin = 0x100000;
        if (end > PMM_LIMIT) end = PMM_LIMIT;
        if (begin >= end) continue;
        /* Do not silently treat non-WB memory as ordinary RAM. */
        if (!(d->attributes & 8)) goto fail;
        for (uint64_t page = begin; page < end; page += 4096)
            if (!map_page(page)) goto fail;
    }
    if (!mapped_range(info->kernel_base, info->kernel_size)
        || !mapped_range(info->stack_base, info->stack_size)
        || !mapped_range((uintptr_t)info, sizeof(*info))
        || !mapped_range(info->memory_map, info->memory_map_size)) goto fail;
    for (uint64_t i = 0; i < paging_table_count; ++i)
        if (!mapped_range(table_pages[i], 4096)) goto fail;

    /* Both stacks have separately reserved pages on either side. Verify each
     * guard is mapped before removing it; no live stack bytes are sacrificed.
     */
    const uint64_t guards[] = {info->stack_base - 4096, info->stack_base + info->stack_size,
        (uintptr_t)double_fault_guard_low, (uintptr_t)double_fault_guard_high};
    for (unsigned i = 0; i < 4; ++i) {
        if (!mapped_range(guards[i], 4096)) goto fail;
        uint64_t *table = (void *)(uintptr_t)paging_root;
        for (unsigned shift = 39; shift > 12; shift -= 9)
            table = (void *)(uintptr_t)(table[(guards[i] >> shift) & 511] & ADDRESS_MASK);
        table[(guards[i] >> 12) & 511] = 0;
    }

    paging_previous_cr3 = x86_read_cr3();
    x86_write_msr(0xc0000080, x86_read_msr(0xc0000080) | (UINT64_C(1) << 11)); /* EFER.NXE */
    x86_write_cr0(x86_read_cr0() | (UINT64_C(1) << 16)); /* Enforce read-only pages at CPL0 too. */
    /* Invalidate inherited global translations too, then restore CR4. */
    uint64_t no_global = cr4 & ~UINT64_C(0x80);
    x86_write_cr4(no_global);
    x86_write_cr3(paging_root);
    x86_write_cr4(cr4);
    active = true;
    return true;
fail:
    while (paging_table_count) pmm_free(table_pages[--paging_table_count]);
    paging_root = 0;
    return false;
}

static bool dynamic_address(uint64_t address)
{
    uint64_t flags = x86_read_rflags();
    return active && !(flags & 0x200) && !(address & 4095)
        && address >= PAGING_DYNAMIC_BASE
        && address - PAGING_DYNAMIC_BASE < PAGING_DYNAMIC_SIZE;
}

static void release_table(uint64_t address)
{
    for (uint64_t i = 0; i < paging_table_count; ++i) {
        if (table_pages[i] != address) continue;
        table_pages[i] = table_pages[--paging_table_count];
        pmm_free(address);
        return;
    }
}

static uint64_t leaf_flags(unsigned flags)
{
    return 1 | ((flags & PAGING_WRITE) ? 2 : 0) | ((flags & PAGING_EXEC) ? 0 : NX);
}

/* Obtain the existing path, root through PT. */
static bool path(uint64_t address, uint64_t *tables[4], unsigned indices[4])
{
    tables[0] = (void *)(uintptr_t)paging_root;
    for (unsigned level = 0; level < 4; ++level) {
        indices[level] = (address >> (39 - level * 9)) & 511;
        uint64_t entry = tables[level][indices[level]];
        if (!(entry & 1)) return false;
        if (level < 3) tables[level + 1] = (void *)(uintptr_t)(entry & ADDRESS_MASK);
    }
    return true;
}

bool paging_map(uint64_t address, uint64_t physical, unsigned flags)
{
    if (!dynamic_address(address) || (flags & ~(PAGING_WRITE | PAGING_EXEC))
        || !pmm_is_allocated(physical)) return false;
    for (uint64_t i = 0; i < paging_table_count; ++i)
        if (table_pages[i] == physical) return false;
    uint64_t *table = (void *)(uintptr_t)paging_root;
    uint64_t *links[3], created[3];
    unsigned count = 0;
    for (unsigned shift = 39; shift > 12; shift -= 9) {
        uint64_t *entry = &table[(address >> shift) & 511];
        if (!(*entry & 1)) {
            uint64_t child = new_table();
            if (!child) {
                while (count) {
                    --count;
                    *links[count] = 0;
                    x86_invalidate_page(address);
                    release_table(created[count]);
                }
                return false;
            }
            links[count] = entry;
            created[count++] = child;
            *entry = child | FLAGS;
        }
        table = (void *)(uintptr_t)(*entry & ADDRESS_MASK);
    }
    uint64_t *leaf = &table[(address >> 12) & 511];
    if (*leaf & 1) return false; /* Existing leaf implies no new tables above. */
    *leaf = physical | leaf_flags(flags);
    x86_invalidate_page(address);
    return true;
}

bool paging_unmap(uint64_t address)
{
    uint64_t *tables[4];
    unsigned indices[4];
    if (!dynamic_address(address) || !path(address, tables, indices)) return false;
    tables[3][indices[3]] = 0;
    x86_invalidate_page(address);
    for (unsigned level = 3; level > 0; --level) {
        for (unsigned i = 0; i < 512; ++i)
            if (tables[level][i]) return true;
        tables[level - 1][indices[level - 1]] = 0;
        x86_invalidate_page(address);
        release_table((uintptr_t)tables[level]);
    }
    return true;
}

bool paging_protect(uint64_t address, unsigned flags)
{
    uint64_t *tables[4];
    unsigned indices[4];
    if (!dynamic_address(address) || (flags & ~(PAGING_WRITE | PAGING_EXEC))
        || !path(address, tables, indices)) return false;
    uint64_t *leaf = &tables[3][indices[3]];
    *leaf = (*leaf & ADDRESS_MASK) | leaf_flags(flags);
    x86_invalidate_page(address);
    return true;
}

bool paging_query(uint64_t address, uint64_t *physical, unsigned *flags)
{
    uint64_t *tables[4];
    unsigned indices[4];
    if (!dynamic_address(address) || !physical || !flags || !path(address, tables, indices)) return false;
    uint64_t entry = tables[3][indices[3]];
    *physical = entry & ADDRESS_MASK;
    *flags = ((entry & 2) ? PAGING_WRITE : 0) | ((entry & NX) ? 0 : PAGING_EXEC);
    return true;
}
