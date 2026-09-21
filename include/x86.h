#ifndef SHARED_X86_H
#define SHARED_X86_H

#include <stdint.h>

/* Shared by the UEFI loader and kernel; no firmware or kernel dependencies.
 * RSP must be sampled in the caller, not in an out-of-line function's frame.
 */
static inline __attribute__((always_inline)) uint64_t x86_read_rsp(void)
{
    uint64_t value;
    __asm__ volatile ("movq %%rsp, %0" : "=r"(value));
    return value;
}

static inline uint64_t x86_read_rflags(void)
{
    uint64_t value;
    __asm__ volatile ("pushfq; popq %0" : "=r"(value) : : "memory");
    return value;
}

static inline void x86_pause(void)
{
    __asm__ volatile ("pause");
}

static inline _Noreturn void x86_spin_forever(void)
{
    for (;;) x86_pause();
}

static inline void x86_disable_interrupts(void)
{
    __asm__ volatile ("cli" : : : "memory");
}

static inline void x86_enable_interrupts(void)
{
    __asm__ volatile ("sti" : : : "memory");
}

/* Caller must have IF=1 and an enabled interrupt source. */
static inline void x86_idle(void)
{
    __asm__ volatile ("hlt" : : : "memory");
}

static inline uint64_t x86_irq_save(void)
{
    uint64_t flags = x86_read_rflags();
    x86_disable_interrupts();
    return flags;
}

static inline void x86_irq_restore(uint64_t flags)
{
    if (flags & 0x200) x86_enable_interrupts();
}

/* Enable and sleep atomically relative to maskable interrupt delivery. */
static inline void x86_enable_and_idle(void)
{
    __asm__ volatile ("sti; hlt" : : : "memory");
}

static inline void x86_outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline uint8_t x86_inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

typedef struct {
    uint32_t eax, ebx, ecx, edx;
} X86_CPUID_RESULT;

static inline X86_CPUID_RESULT x86_cpuid(uint32_t leaf, uint32_t subleaf)
{
    X86_CPUID_RESULT result;
    __asm__ volatile ("cpuid" : "=a"(result.eax), "=b"(result.ebx),
                      "=c"(result.ecx), "=d"(result.edx) : "a"(leaf), "c"(subleaf));
    return result;
}

static inline uint64_t x86_read_msr(uint32_t index)
{
    uint32_t low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(index));
    return ((uint64_t)high << 32) | low;
}

static inline void x86_write_msr(uint32_t index, uint64_t value)
{
    __asm__ volatile ("wrmsr" : : "a"((uint32_t)value), "d"((uint32_t)(value >> 32)),
                      "c"(index) : "memory");
}

static inline uint64_t x86_read_cr0(void)
{
    uint64_t value;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(value));
    return value;
}

static inline void x86_write_cr0(uint64_t value)
{
    __asm__ volatile ("mov %0, %%cr0" : : "r"(value) : "memory");
}

static inline uint64_t x86_read_cr3(void)
{
    uint64_t value;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(value));
    return value;
}

static inline void x86_write_cr3(uint64_t value)
{
    __asm__ volatile ("mov %0, %%cr3" : : "r"(value) : "memory");
}

static inline uint64_t x86_read_cr4(void)
{
    uint64_t value;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(value));
    return value;
}

static inline void x86_write_cr4(uint64_t value)
{
    __asm__ volatile ("mov %0, %%cr4" : : "r"(value) : "memory");
}

static inline void x86_invalidate_page(uint64_t address)
{
    __asm__ volatile ("invlpg (%0)" : : "r"(address) : "memory");
}

#endif
