#include "load.h"
#include <stdbool.h>

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

static bool valid_elf_header(const ELF_HEADER *header, uintptr_t size)
{
    const uint8_t *ident = header->ident;
    if (ident[0] != 0x7f || ident[1] != 'E' || ident[2] != 'L' || ident[3] != 'F') return false;
    if (ident[4] != 2 || ident[5] != 1 || ident[6] != 1) return false;
    if (header->type != 2 || header->machine != 62 || header->version != 1) return false;
    if (header->ehsize != sizeof(*header) || header->phentsize != sizeof(ELF_PROGRAM)) return false;
    if (header->phnum == 0 || header->phnum > 128) return false;
    if (header->phoff % 8 != 0 || header->phoff > size) return false;
    return header->phnum <= (size - header->phoff) / sizeof(ELF_PROGRAM);
}

static bool valid_segment(const ELF_PROGRAM *segment, uintptr_t file_size)
{
    if (segment->filesz > segment->memsz || segment->offset > file_size) return false;
    if (segment->filesz > file_size - segment->offset) return false;
    if (segment->paddr != segment->vaddr) return false;
    if (segment->vaddr < 0x100000 || segment->vaddr >= 0x100000000ULL) return false;
    if (segment->memsz > 0x100000000ULL - segment->vaddr) return false;
    if (segment->align <= 1) return true;
    if (segment->align & (segment->align - 1)) return false;
    return segment->vaddr % segment->align == segment->offset % segment->align;
}

static bool overlaps_previous_segment(const ELF_PROGRAM *programs, unsigned index)
{
    const ELF_PROGRAM *current = &programs[index];
    for (unsigned i = 0; i < index; ++i) {
        const ELF_PROGRAM *other = &programs[i];
        if (other->type != 1 || !other->memsz) continue;
        if (current->vaddr >= other->vaddr + other->memsz) continue;
        if (other->vaddr < current->vaddr + current->memsz) return true;
    }
    return false;
}

typedef struct {
    uint64_t base, end;
} IMAGE_RANGE;

static EFI_STATUS inspect_segments(const ELF_HEADER *h, uintptr_t size, IMAGE_RANGE *range)
{
    const void *data = h;
    const ELF_PROGRAM *ph = (const void *)((const uint8_t *)data + h->phoff);
    uint64_t low = UINT64_MAX, high = 0;
    int entry_valid = 0;
    for (unsigned i = 0; i < h->phnum; ++i) {
        const ELF_PROGRAM *p = &ph[i];
        if (p->type == 2 || p->type == 3) return EFI_UNSUPPORTED; /* No dynamic linking. */
        if (p->type != 1) continue;
        if (!valid_segment(p, size)) return EFI_LOAD_ERROR;
        if (p->memsz == 0) continue;
        if (overlaps_previous_segment(ph, i)) return EFI_LOAD_ERROR;
        if ((p->flags & 1) && h->entry >= p->vaddr && h->entry - p->vaddr < p->filesz)
            entry_valid = 1;
        uint64_t begin = p->vaddr & ~4095ULL;
        uint64_t end = (p->vaddr + p->memsz + 4095) & ~4095ULL;
        if (begin < low) low = begin;
        if (end > high) high = end;
    }
    /* Bound the initial loader to a 64 MiB image span below 4 GiB. */
    if (!entry_valid || high <= low || high - low > 64 * 1024 * 1024) return EFI_LOAD_ERROR;
    range->base = low;
    range->end = high;
    return EFI_SUCCESS;
}

static void copy_segments(const ELF_HEADER *header, const IMAGE_RANGE *range)
{
    const uint8_t *data = (const void *)header;
    const ELF_PROGRAM *programs = (const void *)(data + header->phoff);
    /* Volatile stores avoid compiler-generated libc calls. */
    volatile uint8_t *memory = (void *)(uintptr_t)range->base;
    for (uint64_t i = 0; i < range->end - range->base; ++i) memory[i] = 0;
    for (unsigned i = 0; i < header->phnum; ++i) {
        const ELF_PROGRAM *segment = &programs[i];
        if (segment->type != 1) continue;
        volatile uint8_t *destination = (void *)(uintptr_t)segment->vaddr;
        for (uint64_t j = 0; j < segment->filesz; ++j)
            destination[j] = data[segment->offset + j];
    }
}

static EFI_STATUS place_kernel(EFI_BOOT_SERVICES *bs, void *data, uintptr_t size, LOADED_KERNEL *kernel)
{
    if (size < sizeof(ELF_HEADER)) return EFI_LOAD_ERROR;
    const ELF_HEADER *header = data;
    if (!valid_elf_header(header, size)) return EFI_LOAD_ERROR;
    IMAGE_RANGE range;
    EFI_STATUS status = inspect_segments(header, size, &range);
    if (EFI_ERROR(status)) return status;
    uint64_t address = range.base;
    uintptr_t pages = (range.end - range.base) / 4096;
    status = bs->AllocatePages(2, 1, pages, &address); /* AllocateAddress, EfiLoaderCode */
    if (EFI_ERROR(status)) return status;
    copy_segments(header, &range);
    kernel->base = address;
    kernel->pages = pages;
    kernel->entry = header->entry;
    return EFI_SUCCESS;
}

static EFI_STATUS read_entire_file(EFI_FILE_PROTOCOL *file, uint8_t *buffer, uintptr_t length)
{
    uintptr_t total = 0;
    while (total < length) {
        uintptr_t count = length - total;
        EFI_STATUS status = file->Read(file, &count, buffer + total);
        if (EFI_ERROR(status)) return status;
        if (count == 0 || count > length - total) return EFI_LOAD_ERROR;
        total += count;
    }
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
    status = read_entire_file(file, buffer, (uintptr_t)length);
    if (EFI_ERROR(status)) goto cleanup;
    status = place_kernel(bs, buffer, (uintptr_t)length, kernel);
cleanup:
    if (buffer) bs->FreePool(buffer);
    if (file) file->Close(file);
    root->Close(root);
    return status;
}
