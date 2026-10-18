#include "context.h"

#include <string.h>

void ts_context_init(ts_context *ctx, void *stack_top, void (*entry)(void)) {
    memset(ctx, 0, sizeof *ctx);
    uintptr_t top = (uintptr_t)stack_top & ~(uintptr_t)15; /* both ABIs want 16-byte alignment */
#if defined(__aarch64__)
    ctx->regs[10] = 0;                 /* x29: frame pointer 0 ends the chain a debugger walks */
    ctx->regs[11] = (uintptr_t)entry;  /* x30: the `ret` in ts_context_switch jumps here */
    ctx->regs[12] = top;               /* sp */
#elif defined(__x86_64__)
    ctx->regs[1] = 0;                  /* rbp */
    ctx->regs[6] = top - 8;            /* rsp: as if a call had just pushed a return address */
    ctx->regs[7] = (uintptr_t)entry;   /* where the jmp lands */
    uint32_t mxcsr;
    uint16_t fpucw;
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    __asm__ volatile("fnstcw %0" : "=m"(fpucw));
    memcpy((char *)ctx->regs + 64, &mxcsr, sizeof mxcsr); /* start with this thread's FP modes */
    memcpy((char *)ctx->regs + 68, &fpucw, sizeof fpucw);
#endif
}
