#ifndef KERNEL_LAYOUT_H
#define KERNEL_LAYOUT_H
/* Page-aligned boundaries defined by linker.ld. */
extern char kernel_text_start[], kernel_text_end[];
extern char kernel_rodata_start[], kernel_rodata_end[];
extern char double_fault_guard_low[], double_fault_guard_high[];
#endif
