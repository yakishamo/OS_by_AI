.section .text,"ax",@progbits
.global tables_load
.type tables_load,@function
tables_load:
    cli
    lgdt (%rdi)
    pushq $0x08
    leaq 1f(%rip), %rax
    pushq %rax
    lretq
1:  movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    xor %eax, %eax
    movw %ax, %fs
    movw %ax, %gs
    movw $0x18, %ax
    ltr %ax
    lidt (%rsi)
    ret
.size tables_load, .-tables_load

/* Hardware pushes an error code for these exceptions. Other vectors need a
 * synthetic zero so the C frame is uniform. External IRQs remain disabled.
 */
.altmacro
.macro ISR number
.global isr_\number
isr_\number:
    .if (\number != 8) && (\number != 10) && (\number != 11) && (\number != 12) && (\number != 13) && (\number != 14) && (\number != 17) && (\number != 21) && (\number != 29) && (\number != 30)
    pushq $0
    .endif
    pushq $\number
    jmp exception_common
.endm
.set vector, 0
.rept 256
ISR %vector
.set vector, vector+1
.endr

exception_common:
    cld
    movq %cr2, %rsi
    movq %rsp, %rdi
    andq $-16, %rsp
    call exception_panic

.global exception_halt
.type exception_halt,@function
exception_halt:
    cli
2:  hlt
    jmp 2b
.size exception_halt, .-exception_halt

.section .rodata,"a",@progbits
.balign 8
.global isr_table
isr_table:
.macro ISR_POINTER number
    .quad isr_\number
.endm
.set vector, 0
.rept 256
ISR_POINTER %vector
.set vector, vector+1
.endr
.section .note.GNU-stack,"",@progbits
