#include "tables.h"
#include "serial.h"
#include <stddef.h>

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} TABLE_POINTER;
typedef struct __attribute__((packed)) {
    uint16_t offset_low, selector;
    uint8_t ist, attributes;
    uint16_t offset_mid;
    uint32_t offset_high, reserved;
} IDT_GATE;
typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp[3], reserved1, ist[7], reserved2;
    uint16_t reserved3, iomap_base;
} TSS;
_Static_assert(sizeof(TABLE_POINTER) == 10, "Descriptor pointer ABI");
_Static_assert(sizeof(IDT_GATE) == 16, "IDT gate ABI");
_Static_assert(sizeof(TSS) == 104, "64-bit TSS ABI");
_Static_assert(offsetof(TSS, ist) == 36, "TSS IST offset");
_Static_assert(sizeof(EXCEPTION_FRAME) == 56, "Exception stack ABI");

/* Writable because LTR marks the TSS descriptor busy. */
uint64_t kernel_gdt[5] __attribute__((aligned(16)));
IDT_GATE kernel_idt[256] __attribute__((aligned(16)));
TSS kernel_tss;
uint8_t double_fault_stack[16384] __attribute__((aligned(16)));
extern void (*const isr_table[256])(void);
extern void tables_load(const TABLE_POINTER *, const TABLE_POINTER *);
extern _Noreturn void exception_halt(void);

void tables_init(uint64_t stack_top)
{
    kernel_gdt[0] = 0;
    kernel_gdt[1] = UINT64_C(0x00af9a000000ffff); /* 0x08: ring-0 64-bit code */
    kernel_gdt[2] = UINT64_C(0x00cf92000000ffff); /* 0x10: ring-0 data */
    kernel_tss.rsp[0] = stack_top;
    kernel_tss.ist[0] = (uint64_t)(double_fault_stack + sizeof(double_fault_stack));
    kernel_tss.iomap_base = sizeof(kernel_tss); /* No I/O permission bitmap. */
    uint64_t base = (uint64_t)&kernel_tss;
    kernel_gdt[3] = (sizeof(kernel_tss) - 1) | ((base & 0xffffff) << 16)
                  | (UINT64_C(0x89) << 40) | (((base >> 24) & 0xff) << 56);
    kernel_gdt[4] = base >> 32;
    for (unsigned i = 0; i < 256; ++i) {
        uint64_t target = (uint64_t)isr_table[i];
        kernel_idt[i].offset_low = (uint16_t)target;
        kernel_idt[i].selector = 0x08;
        kernel_idt[i].ist = i == 8 ? 1 : 0; /* #DF has its own stack. */
        kernel_idt[i].attributes = 0x8e; /* Present, DPL0, interrupt gate. */
        kernel_idt[i].offset_mid = (uint16_t)(target >> 16);
        kernel_idt[i].offset_high = (uint32_t)(target >> 32);
        kernel_idt[i].reserved = 0;
    }
    TABLE_POINTER gdtr = {sizeof(kernel_gdt) - 1, (uint64_t)kernel_gdt};
    TABLE_POINTER idtr = {sizeof(kernel_idt) - 1, (uint64_t)kernel_idt};
    tables_load(&gdtr, &idtr);
}

static void print_hex(const char *label, uint64_t value)
{
    char text[] = "0x0000000000000000\n";
    const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; ++i) text[2 + i] = digits[(value >> (60 - i * 4)) & 15];
    serial_write(label);
    serial_write(text);
}

_Noreturn void exception_panic(const EXCEPTION_FRAME *frame, uint64_t cr2)
{
    /* Called with interrupts disabled and a SysV-aligned stack. Fatal only;
     * the stub need not restore registers or return to the faulting context.
     */
    serial_write("\nEXCEPTION: fatal\n");
    print_hex("vector=", frame->vector);
    print_hex("error=", frame->error);
    print_hex("rip=", frame->rip);
    print_hex("cs=", frame->cs);
    print_hex("rflags=", frame->rflags);
    print_hex("rsp=", frame->rsp);
    if (frame->vector == 14) print_hex("cr2=", cr2);
    serial_flush();
    exception_halt();
}
