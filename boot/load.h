#ifndef BOOT_LOAD_H
#define BOOT_LOAD_H
#include "efi.h"

typedef struct {
    uint64_t base;
    uintptr_t pages;
    uint64_t entry;
} LOADED_KERNEL;

EFI_STATUS load_kernel(EFI_BOOT_SERVICES *bs, EFI_HANDLE image, LOADED_KERNEL *kernel);
#endif
