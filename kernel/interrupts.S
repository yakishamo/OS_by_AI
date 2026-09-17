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
 * synthetic zero so the C frame is uniform. PIC IRQs use a returning path.
 */
.altmacro
.macro ISR number
.global isr_\number
isr_\number:
    .if (\number != 8) && (\number != 10) && (\number != 11) && (\number != 12) && (\number != 13) && (\number != 14) && (\number != 17) && (\number != 21) && (\number != 29) && (\number != 30)
    pushq $0
    .endif
    pushq $\number
    .if (\number >= 32) && (\number < 48)
    jmp irq_common
    .else
    jmp exception_common
    .endif
.endm
.set vector, 0
.rept 256
ISR %vector
.set vector, vector+1
.endr

/* Save every GPR, retain the interrupted stack, align C's call stack, then
 * restore registers and the hardware frame. -mgeneral-regs-only excludes SIMD.
 */
irq_common:
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %rbx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    cld
    movq 120(%rsp), %rdi
    movq %rsp, %rbx
    andq $-16, %rsp
    call irq_dispatch
    movq %rbx, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rbx
    popq %rdx
    popq %rcx
    popq %rax
    addq $16, %rsp
    iretq

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

/* Verify preservation across a real interrupt, including DF restoration.
 * Caller enters with IF=0; success/failure both return with IF=0 and DF=0.
 */
.global timer_check_registers
.type timer_check_registers,@function
timer_check_registers:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq $1, %rax
    movq $2, %rcx
    movq $3, %rdx
    movq $4, %rbx
    movq $5, %rbp
    movq $6, %rsi
    movq $7, %rdi
    movq $8, %r8
    movq $9, %r9
    movq $10, %r10
    movq $11, %r11
    movq $12, %r12
    movq $13, %r13
    movq $14, %r14
    movq $15, %r15
    std
    stc
    sti
    hlt
    cli
    pushfq
    cld
.macro CHECK_REGISTER register, value
    cmpq $\value, %\register
    jne 3f
.endm
    CHECK_REGISTER rax, 1
    CHECK_REGISTER rcx, 2
    CHECK_REGISTER rdx, 3
    CHECK_REGISTER rbx, 4
    CHECK_REGISTER rbp, 5
    CHECK_REGISTER rsi, 6
    CHECK_REGISTER rdi, 7
    CHECK_REGISTER r8, 8
    CHECK_REGISTER r9, 9
    CHECK_REGISTER r10, 10
    CHECK_REGISTER r11, 11
    CHECK_REGISTER r12, 12
    CHECK_REGISTER r13, 13
    CHECK_REGISTER r14, 14
    CHECK_REGISTER r15, 15
    popq %rax
    andq $0x401, %rax
    cmpq $0x401, %rax
    sete %al
    movzbl %al, %eax
    jmp 4f
3:  addq $8, %rsp
    xor %eax, %eax
4:  popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret
.size timer_check_registers, .-timer_check_registers

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
