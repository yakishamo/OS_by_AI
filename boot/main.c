#include "../include/x86.h"
#include "load.h"
#include "../include/boot_info.h"

volatile EFI_STATUS boot_exit_failure;

/* Serial may be unavailable: report failures through the UEFI text console. */
static EFI_STATUS report_error(EFI_SYSTEM_TABLE *table, const char *operation, EFI_STATUS status)
{
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *output = table->StdErr ? table->StdErr : table->ConOut;
    if (output != NULL) {
        uint16_t prefix[] = u"\r\nBOOT ERROR: ";
        output->OutputString(output, prefix);
        for (; *operation; ++operation) {
            uint16_t character[] = {(uint8_t)*operation, 0};
            output->OutputString(output, character);
        }
        uint16_t code[] = u" status=0x0000000000000000\r\n";
        const char digits[] = "0123456789abcdef";
        for (unsigned i = 0; i < 16; ++i) {
            code[10 + i] = (uint16_t)digits[(status >> ((15 - i) * 4)) & 15];
        }
        output->OutputString(output, code);
    }
    return status;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table)
{
    EFI_BOOT_SERVICES *services = system_table->BootServices;
    /* Disable the firmware watchdog before handing control to the kernel. */
    EFI_STATUS status = services->SetWatchdogTimer(0, 0, 0, NULL);
    if (EFI_ERROR(status)) {
        return report_error(system_table, "SetWatchdogTimer", status);
    }

    EFI_GUID serial_guid = {0xbb25cf6f, 0xf1d4, 0x11d2,
                            {0x9a, 0x0c, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0xfd}};
    void *interface = NULL;
    /* One serial device in our QEMU machine; choose the first instance. */
    status = services->LocateProtocol(&serial_guid, NULL, &interface);
    if (EFI_ERROR(status)) {
        return report_error(system_table, "LocateProtocol(Serial IO)", status);
    }
    if (interface == NULL) {
        return report_error(system_table, "LocateProtocol(Serial IO)", EFI_DEVICE_ERROR);
    }
    EFI_SERIAL_IO_PROTOCOL *serial = interface;
    /* Default FIFO depth; 10 ms timeout per character, 115200 baud, 8N1. */
    status = serial->SetAttributes(serial, 115200, 0, 10000, NoParity, 8, OneStopBit);
    if (EFI_ERROR(status)) {
        return report_error(system_table, "Serial IO SetAttributes", status);
    }
    static char banner[] =
        "\r\nBOOT: UEFI x86_64 C entry\r\n"
        "BOOT: EFI_SERIAL_IO_PROTOCOL via LocateProtocol\r\n"
        "BOOT: serial ready (115200 8N1)\r\n"
        "BOOT: loading kernel.elf\r\n";
    uintptr_t size = sizeof(banner) - 1;
    status = serial->Write(serial, &size, banner);
    if (!EFI_ERROR(status) && size != sizeof(banner) - 1) {
        status = EFI_DEVICE_ERROR;
    }
    if (EFI_ERROR(status)) {
        return report_error(system_table, "Serial IO Write", status);
    }

    LOADED_KERNEL kernel;
    status = load_kernel(services, image_handle, &kernel);
    if (EFI_ERROR(status)) return report_error(system_table, "load kernel.elf", status);

    char entry[] = "BOOT: kernel entry=0x0000000000000000\r\n";
    const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; ++i)
        entry[sizeof("BOOT: kernel entry=0x") - 1 + i] = digits[(kernel.entry >> ((15 - i) * 4)) & 15];
    size = sizeof(entry) - 1;
    status = serial->Write(serial, &size, entry);
    if (EFI_ERROR(status) || size != sizeof(entry) - 1) {
        services->FreePages(kernel.base, kernel.pages);
        return report_error(system_table, "Serial IO Write", EFI_ERROR(status) ? status : EFI_DEVICE_ERROR);
    }

    /* One owned allocation: information + 64 KiB map + guard + 64 KiB stack + guard.
     * All remain EfiLoaderData in the final map; the kernel must retain them.
     */
    const uintptr_t handoff_pages = 35;
    const uintptr_t map_capacity = 16 * 4096;
    uint64_t handoff_base = 0;
    status = services->AllocatePages(0, 2, handoff_pages, &handoff_base);
    if (EFI_ERROR(status)) {
        services->FreePages(kernel.base, kernel.pages);
        return report_error(system_table, "AllocatePages(boot information)", status);
    }
    volatile uint8_t *clear = (void *)(uintptr_t)handoff_base;
    for (uintptr_t i = 0; i < handoff_pages * 4096; ++i) clear[i] = 0;
    BOOT_INFO *info = (void *)(uintptr_t)handoff_base;
    info->magic = BOOT_INFO_MAGIC;
    info->version = BOOT_INFO_VERSION;
    info->size = sizeof(*info);
    info->memory_map = handoff_base + 4096;
    info->kernel_base = kernel.base;
    info->kernel_size = kernel.pages * 4096;
    info->stack_base = info->memory_map + map_capacity + 4096;
    info->stack_size = 16 * 4096;

    /* Only the final GetMemoryMap and ExitBootServices calls follow. */
    uintptr_t map_key, descriptor_size;
    uint32_t descriptor_version;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        size = map_capacity;
        status = services->GetMemoryMap(&size, (void *)(uintptr_t)info->memory_map, &map_key, &descriptor_size, &descriptor_version);
        if (EFI_ERROR(status)) {
            if (attempt == 0) {
                services->FreePages(handoff_base, handoff_pages);
                services->FreePages(kernel.base, kernel.pages);
                return report_error(system_table, "GetMemoryMap", status);
            }
            break;
        }
        info->memory_map_size = size;
        info->descriptor_size = descriptor_size;
        info->descriptor_version = descriptor_version;
        status = services->ExitBootServices(image_handle, map_key);
        if (status == EFI_SUCCESS) {
            info->flags = BOOT_SERVICES_EXITED;
            typedef void (__attribute__((sysv_abi)) *KERNEL_ENTRY)(const BOOT_INFO *, uint64_t);
            x86_disable_interrupts();
            ((KERNEL_ENTRY)(uintptr_t)kernel.entry)(info, info->stack_base + info->stack_size);
            /* A kernel must never return to retired boot services. */
            status = EFI_LOAD_ERROR;
            break;
        }
        if (status != EFI_INVALID_PARAMETER) break;
    }
    /* After an attempted exit firmware may be partially shut down. */
    boot_exit_failure = status;
    x86_disable_interrupts();
    x86_spin_forever();
}
