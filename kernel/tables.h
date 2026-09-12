#ifndef KERNEL_TABLES_H
#define KERNEL_TABLES_H
#include <stdint.h>

void tables_init(uint64_t stack_top);

/* Normalized fatal exception frame: no return/IRET support at this stage. */
typedef struct {
    uint64_t vector, error, rip, cs, rflags, rsp, ss;
} EXCEPTION_FRAME;
_Noreturn void exception_panic(const EXCEPTION_FRAME *frame, uint64_t cr2);

#endif
