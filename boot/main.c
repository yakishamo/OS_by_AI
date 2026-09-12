#include "efi.h"

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
    (void)image_handle;
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
        "BOOT: kernel.elf loading not implemented yet\r\n";
    uintptr_t size = sizeof(banner) - 1;
    status = serial->Write(serial, &size, banner);
    if (!EFI_ERROR(status) && size != sizeof(banner) - 1) {
        status = EFI_DEVICE_ERROR;
    }
    if (EFI_ERROR(status)) {
        return report_error(system_table, "Serial IO Write", status);
    }

    /* Build split only: remain in UEFI until the ELF loader is implemented. */
    for (;;) {
        services->Stall(1000000);
    }
}
