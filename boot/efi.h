#ifndef BOOT_EFI_H
#define BOOT_EFI_H

#include <stddef.h>
#include <stdint.h>

/* Minimal x64 UEFI ABI declarations. Unused fields retain their ABI slots.
 * Extend with real prototypes before calling additional services.
 */
#define EFIAPI __attribute__((ms_abi))
typedef uintptr_t EFI_STATUS;
typedef void *EFI_HANDLE;
#define EFI_SUCCESS ((EFI_STATUS)0)
#define EFI_ERROR_BIT ((EFI_STATUS)1 << 63)
#define EFI_ERROR(status) (((status) & EFI_ERROR_BIT) != 0)
#define EFI_DEVICE_ERROR (EFI_ERROR_BIT | 7)
#define EFI_TIMEOUT (EFI_ERROR_BIT | 18)

typedef struct {
    uint32_t Data1;
    uint16_t Data2, Data3;
    uint8_t Data4[8];
} EFI_GUID;

typedef struct {
    uint64_t Signature;
    uint32_t Revision, HeaderSize, CRC32, Reserved;
} EFI_TABLE_HEADER;

typedef void (EFIAPI *EFI_UNUSED_SERVICE)(void);
typedef struct {
    EFI_TABLE_HEADER Hdr;
    EFI_UNUSED_SERVICE RaiseTPL, RestoreTPL;
    EFI_UNUSED_SERVICE AllocatePages, FreePages, GetMemoryMap, AllocatePool, FreePool;
    EFI_UNUSED_SERVICE CreateEvent, SetTimer, WaitForEvent, SignalEvent, CloseEvent, CheckEvent;
    EFI_UNUSED_SERVICE InstallProtocolInterface, ReinstallProtocolInterface;
    EFI_UNUSED_SERVICE UninstallProtocolInterface, HandleProtocol;
    void *Reserved;
    EFI_UNUSED_SERVICE RegisterProtocolNotify, LocateHandle, LocateDevicePath;
    EFI_UNUSED_SERVICE InstallConfigurationTable, LoadImage, StartImage, Exit;
    EFI_UNUSED_SERVICE UnloadImage, ExitBootServices, GetNextMonotonicCount;
    EFI_STATUS (EFIAPI *Stall)(uintptr_t Microseconds);
    EFI_STATUS (EFIAPI *SetWatchdogTimer)(uintptr_t Timeout, uint64_t WatchdogCode,
                                         uintptr_t DataSize, uint16_t *WatchdogData);
    EFI_UNUSED_SERVICE ConnectController, DisconnectController, OpenProtocol;
    EFI_UNUSED_SERVICE CloseProtocol, OpenProtocolInformation, ProtocolsPerHandle;
    EFI_UNUSED_SERVICE LocateHandleBuffer;
    EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *Protocol, void *Registration, void **Interface);
    /* Remaining services are not accessed at this stage. */
} EFI_BOOT_SERVICES;

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_UNUSED_SERVICE Reset;
    EFI_STATUS (EFIAPI *OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, uint16_t *String);
};

typedef struct {
    EFI_TABLE_HEADER Hdr;
    uint16_t *FirmwareVendor;
    uint32_t FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    void *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    void *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    uintptr_t NumberOfTableEntries;
    void *ConfigurationTable;
} EFI_SYSTEM_TABLE;

typedef enum { DefaultParity, NoParity, EvenParity, OddParity, MarkParity, SpaceParity } EFI_PARITY_TYPE;
typedef enum { DefaultStopBits, OneStopBit, OneFiveStopBits, TwoStopBits } EFI_STOP_BITS_TYPE;

typedef struct EFI_SERIAL_IO_PROTOCOL EFI_SERIAL_IO_PROTOCOL;
struct EFI_SERIAL_IO_PROTOCOL {
    uint32_t Revision;
    EFI_STATUS (EFIAPI *Reset)(EFI_SERIAL_IO_PROTOCOL *This);
    EFI_STATUS (EFIAPI *SetAttributes)(EFI_SERIAL_IO_PROTOCOL *This, uint64_t BaudRate,
                                      uint32_t ReceiveFifoDepth, uint32_t Timeout,
                                      EFI_PARITY_TYPE Parity, uint8_t DataBits,
                                      EFI_STOP_BITS_TYPE StopBits);
    EFI_STATUS (EFIAPI *SetControl)(EFI_SERIAL_IO_PROTOCOL *This, uint32_t Control);
    EFI_STATUS (EFIAPI *GetControl)(EFI_SERIAL_IO_PROTOCOL *This, uint32_t *Control);
    EFI_STATUS (EFIAPI *Write)(EFI_SERIAL_IO_PROTOCOL *This, uintptr_t *BufferSize, void *Buffer);
    EFI_STATUS (EFIAPI *Read)(EFI_SERIAL_IO_PROTOCOL *This, uintptr_t *BufferSize, void *Buffer);
    void *Mode;
    /* Revision 1.1's DeviceTypeGuid is not accessed. */
};

_Static_assert(sizeof(uintptr_t) == 8, "x64 UEFI requires 64-bit UINTN");
_Static_assert(sizeof(EFI_GUID) == 16, "UEFI GUID layout");
_Static_assert(sizeof(EFI_TABLE_HEADER) == 24, "UEFI table header layout");
_Static_assert(offsetof(EFI_SYSTEM_TABLE, ConOut) == 64, "UEFI ConOut offset");
_Static_assert(offsetof(EFI_SYSTEM_TABLE, BootServices) == 96, "UEFI BootServices offset");
_Static_assert(offsetof(EFI_BOOT_SERVICES, SetWatchdogTimer) == 256, "UEFI watchdog offset");
_Static_assert(offsetof(EFI_BOOT_SERVICES, LocateProtocol) == 320, "UEFI LocateProtocol offset");
_Static_assert(offsetof(EFI_SERIAL_IO_PROTOCOL, Write) == 40, "UEFI Serial Write offset");
_Static_assert(offsetof(EFI_SERIAL_IO_PROTOCOL, Read) == 48, "UEFI Serial Read offset");

#endif
