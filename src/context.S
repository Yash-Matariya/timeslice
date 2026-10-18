/*
 * void ts_context_switch(ts_context *from, ts_context *to)
 *
 * Store the callee-saved registers in *from, load them from *to, and return.
 * The return lands wherever *to last called ts_context_switch, or, for a
 * brand-new thread, at the entry point ts_context_init planted.
 */
#if defined(__APPLE__)
#define SYM(name) _##name
#else
#define SYM(name) name
#endif

    .text

#if defined(__aarch64__)

    .p2align 2
    .globl SYM(ts_context_switch)
#if defined(__ELF__)
    .type SYM(ts_context_switch), %function
#endif
SYM(ts_context_switch):
    /* save into *from (x0) */
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]     /* frame pointer, and the return address */
    mov x9, sp
    str x9, [x0, #96]
    stp d8, d9, [x0, #104]
    stp d10, d11, [x0, #120]
    stp d12, d13, [x0, #136]
    stp d14, d15, [x0, #152]

    /* load from *to (x1) */
    ldp x19, x20, [x1, #0]
    ldp x21, x22, [x1, #16]
    ldp x23, x24, [x1, #32]
    ldp x25, x26, [x1, #48]
    ldp x27, x28, [x1, #64]
    ldp x29, x30, [x1, #80]
    ldr x9, [x1, #96]
    mov sp, x9
    ldp d8, d9, [x1, #104]
    ldp d10, d11, [x1, #120]
    ldp d12, d13, [x1, #136]
    ldp d14, d15, [x1, #152]
    ret                         /* to x30: the other thread's return address */

#elif defined(__x86_64__)

    .p2align 4
    .globl SYM(ts_context_switch)
#if defined(__ELF__)
    .type SYM(ts_context_switch), @function
#endif
SYM(ts_context_switch):
    /* save into *from (rdi). The return address is on top of the stack. */
    movq %rbx, 0(%rdi)
    movq %rbp, 8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    movq (%rsp), %rax
    movq %rax, 56(%rdi)         /* where to resume */
    leaq 8(%rsp), %rax
    movq %rax, 48(%rdi)         /* the stack as it will be after returning */
    stmxcsr 64(%rdi)
    fnstcw 68(%rdi)

    /* load from *to (rsi) */
    movq 0(%rsi), %rbx
    movq 8(%rsi), %rbp
    movq 16(%rsi), %r12
    movq 24(%rsi), %r13
    movq 32(%rsi), %r14
    movq 40(%rsi), %r15
    ldmxcsr 64(%rsi)
    fldcw 68(%rsi)
    movq 48(%rsi), %rsp
    jmpq *56(%rsi)

#else
#error "timeslice has a context switch for arm64 and x86-64 only"
#endif

#if defined(__linux__) && defined(__ELF__)
    .section .note.GNU-stack,"",%progbits
#endif
