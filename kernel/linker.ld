OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(kernel_entry)

PHDRS
{
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    /* Initial fixed load address. The future loader must reserve these pages. */
    . = 0x100000;
    kernel_text_start = .;
    .text : { KEEP(*(.text.entry)) *(.text .text.*) } :text
    . = ALIGN(0x1000);
    kernel_text_end = .;
    kernel_rodata_start = .;
    .rodata : { *(.rodata .rodata.*) } :rodata
    . = ALIGN(0x1000);
    kernel_rodata_end = .;
    .data : { *(.data .data.*) } :data
    .bss : { *(.bss .bss.*) *(COMMON) } :data
    .stacks ALIGN(0x1000) (NOLOAD) : {
        double_fault_guard_low = .;
        . += 0x1000;
        *(.df_stack)
        double_fault_guard_high = .;
        ASSERT(double_fault_guard_high - double_fault_guard_low == 0x5000, "DF stack size/alignment");
        . += 0x1000;
    } :data
    /DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) }
}
