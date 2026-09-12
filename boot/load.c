#include "load.h"

typedef struct {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} ELF_HEADER;
typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} ELF_PROGRAM;
_Static_assert(sizeof(ELF_HEADER) == 64, "ELF64 header");
_Static_assert(sizeof(ELF_PROGRAM) == 56, "ELF64 program header");

static EFI_STATUS place_kernel(EFI_BOOT_SERVICES *bs, void *data, uintptr_t size, LOADED_KERNEL *kernel)
{
    if (size < sizeof(ELF_HEADER)) return EFI_LOAD_ERROR;
    const ELF_HEADER *h = data;
    if (h->ident[0] != 0x7f || h->ident[1] != 'E' || h->ident[2] != 'L' || h->ident[3] != 'F'
        || h->ident[4] != 2 || h->ident[5] != 1 || h->ident[6] != 1
        || h->type != 2 || h->machine != 62 || h->version != 1
        || h->ehsize != sizeof(*h) || h->phentsize != sizeof(ELF_PROGRAM)
        || h->phnum == 0 || h->phnum > 128 || h->phoff % 8 != 0
        || h->phoff > size || h->phnum > (size - h->phoff) / sizeof(ELF_PROGRAM))
        return EFI_LOAD_ERROR;
    const ELF_PROGRAM *ph = (const void *)((const uint8_t *)data + h->phoff);
    uint64_t low = UINT64_MAX, high = 0;
    int entry_valid = 0;
    for (unsigned i = 0; i < h->phnum; ++i) {
        const ELF_PROGRAM *p = &ph[i];
        if (p->type == 2 || p->type == 3) return EFI_UNSUPPORTED; /* No dynamic linking. */
        if (p->type != 1) continue;
        if (p->filesz > p->memsz || p->offset > size || p->filesz > size - p->offset
            || p->paddr != p->vaddr || p->vaddr < 0x100000 || p->vaddr >= 0x100000000ULL
            || p->memsz > 0x100000000ULL - p->vaddr
            || (p->align > 1 && ((p->align & (p->align - 1)) != 0
                || p->vaddr % p->align != p->offset % p->align))) return EFI_LOAD_ERROR;
        if (p->memsz == 0) continue;
        for (unsigned j = 0; j < i; ++j) {
            const ELF_PROGRAM *q = &ph[j];
            if (q->type == 1 && q->memsz && p->vaddr < q->vaddr + q->memsz
                && q->vaddr < p->vaddr + p->memsz) return EFI_LOAD_ERROR;
        }
        if ((p->flags & 1) && h->entry >= p->vaddr && h->entry - p->vaddr < p->filesz)
            entry_valid = 1;
        uint64_t begin = p->vaddr & ~4095ULL;
        uint64_t end = (p->vaddr + p->memsz + 4095) & ~4095ULL;
        if (begin < low) low = begin;
        if (end > high) high = end;
    }
    /* Bound the initial loader to a 64 MiB image span below 4 GiB. */
    if (!entry_valid || high <= low || high - low > 64 * 1024 * 1024) return EFI_LOAD_ERROR;
    uint64_t address = low;
    uintptr_t pages = (high - low) / 4096;
    EFI_STATUS status = bs->AllocatePages(2, 1, pages, &address); /* AllocateAddress, EfiLoaderCode */
    if (EFI_ERROR(status)) return status;
    /* Volatile stores avoid compiler-generated libc calls in this freestanding loader. */
    volatile uint8_t *memory = (void *)(uintptr_t)address;
    for (uintptr_t i = 0; i < high - low; ++i) memory[i] = 0;
    for (unsigned i = 0; i < h->phnum; ++i) {
        const ELF_PROGRAM *p = &ph[i];
        if (p->type != 1) continue;
        volatile uint8_t *dest = (void *)(uintptr_t)p->vaddr;
        const uint8_t *src = (const uint8_t *)data + p->offset;
        for (uint64_t j = 0; j < p->filesz; ++j) dest[j] = src[j];
    }
    kernel->base = address;
    kernel->pages = pages;
    kernel->entry = h->entry;
    return EFI_SUCCESS;
}

EFI_STATUS load_kernel(EFI_BOOT_SERVICES *bs, EFI_HANDLE image, LOADED_KERNEL *kernel)
{
    EFI_GUID loaded_guid = {0x5b1b31a1, 0x9562, 0x11d2, {0x8e,0x3f,0,0xa0,0xc9,0x69,0x72,0x3b}};
    EFI_GUID fs_guid = {0x964e5b22, 0x6459, 0x11d2, {0x8e,0x39,0,0xa0,0xc9,0x69,0x72,0x3b}};
    void *interface = NULL, *buffer = NULL;
    EFI_FILE_PROTOCOL *root = NULL, *file = NULL;
    EFI_STATUS status = bs->HandleProtocol(image, &loaded_guid, &interface);
    if (EFI_ERROR(status)) return status;
    EFI_LOADED_IMAGE_PROTOCOL *loaded = interface;
    status = bs->HandleProtocol(loaded->DeviceHandle, &fs_guid, &interface);
    if (EFI_ERROR(status)) return status;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = interface;
    status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status)) return status;
    uint16_t path[] = u"\\kernel.elf";
    status = root->Open(root, &file, path, 1, 0); /* Read only. */
    if (EFI_ERROR(status)) goto cleanup;
    status = file->SetPosition(file, UINT64_MAX);
    if (EFI_ERROR(status)) goto cleanup;
    uint64_t length;
    status = file->GetPosition(file, &length);
    if (EFI_ERROR(status)) goto cleanup;
    if (length < sizeof(ELF_HEADER) || length > 16 * 1024 * 1024) {
        status = EFI_LOAD_ERROR;
        goto cleanup;
    }
    status = file->SetPosition(file, 0);
    if (EFI_ERROR(status)) goto cleanup;
    status = bs->AllocatePool(2, (uintptr_t)length, &buffer); /* EfiLoaderData */
    if (EFI_ERROR(status)) goto cleanup;
    uintptr_t total = 0;
    while (total < length) {
        uintptr_t count = length - total;
        status = file->Read(file, &count, (uint8_t *)buffer + total);
        if (EFI_ERROR(status)) goto cleanup;
        if (count == 0 || count > length - total) { status = EFI_LOAD_ERROR; goto cleanup; }
        total += count;
    }
    status = place_kernel(bs, buffer, total, kernel);
cleanup:
    if (buffer) bs->FreePool(buffer);
    if (file) file->Close(file);
    root->Close(root);
    return status;
}
