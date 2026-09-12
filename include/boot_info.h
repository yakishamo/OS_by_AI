#ifndef SHARED_BOOT_INFO_H
#define SHARED_BOOT_INFO_H

#include <stddef.h>
#include <stdint.h>

/* Versioned loader/kernel contract. Addresses are physical and identity mapped
 * at entry. This header deliberately has no dependency on UEFI interfaces.
 */
#define BOOT_INFO_MAGIC UINT64_C(0x4f5342494e464f31)
#define BOOT_INFO_VERSION 1
#define BOOT_SERVICES_EXITED UINT64_C(1)

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t size;
    uint64_t memory_map;
    uint64_t memory_map_size;
    uint64_t descriptor_size;
    uint32_t descriptor_version;
    uint32_t reserved;
    uint64_t kernel_base;
    uint64_t kernel_size;
    uint64_t stack_base;
    uint64_t stack_size;
    uint64_t flags;
} BOOT_INFO;

/* Descriptor prefix, not the stride: advance by BOOT_INFO.descriptor_size. */
typedef struct {
    uint32_t type;
    uint32_t padding;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attributes;
} BOOT_MEMORY_DESCRIPTOR;

_Static_assert(sizeof(BOOT_INFO) == 88, "Boot information ABI size");
_Static_assert(offsetof(BOOT_INFO, stack_base) == 64, "Boot information ABI stack");
_Static_assert(sizeof(BOOT_MEMORY_DESCRIPTOR) == 40, "Memory descriptor prefix");

#endif
