/* System V AMD64: RDI = BOOT_INFO *, RSI = exclusive stack top.
 * Do not touch the inherited stack. Establish alignment before calling C.
 */
.section .text.entry,"ax",@progbits
.global kernel_entry
.type kernel_entry,@function
kernel_entry:
    cli
    cld
    movq %rsi, %rsp
    andq $-16, %rsp
    xorq %rbp, %rbp
    movq %rsp, kernel_initial_rsp(%rip)
    call kernel_main
1:  cli
    pause
    jmp 1b
.size kernel_entry, .-kernel_entry

.global kernel_halt
.type kernel_halt,@function
kernel_halt:
    hlt
    jmp kernel_halt
.size kernel_halt, .-kernel_halt

/* QEMU-only fault fixture entry: overflow the real kernel stack into its guard. */
.section .text,"ax",@progbits
.global paging_test_stack_overflow
.type paging_test_stack_overflow,@function
paging_test_stack_overflow:
    movq kernel_boot_info(%rip), %rax
    movq 64(%rax), %rsp
    pushq %rax
    ud2
.size paging_test_stack_overflow, .-paging_test_stack_overflow

.section .note.GNU-stack,"",@progbits
