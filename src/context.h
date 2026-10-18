/*
 * Saving one thread's registers and loading another's.
 *
 * A context switch only has to save the registers the calling convention says
 * a function must preserve ("callee-saved"). Every other register is already
 * assumed clobbered by the call to ts_context_switch, so the compiler has
 * saved anything it still needs before calling. That is why this is 21 words
 * on arm64 and 9 on x86-64, not the whole register file.
 *
 *   arm64   x19-x28, the frame pointer x29, the link register x30, sp, d8-d15
 *   x86-64  rbx, rbp, r12-r15, rsp, the resume address, and the MXCSR and
 *           x87 control words (rounding modes are callee-saved too)
 */
#ifndef TS_CONTEXT_H
#define TS_CONTEXT_H

#include <stdint.h>

typedef struct {
    uint64_t regs[24]; /* layout owned by context.S */
} ts_context;

/* Save the current registers into *from, load *to, and continue wherever *to
 * left off. Returns only when something later switches back to *from. */
void ts_context_switch(ts_context *from, ts_context *to);

/* Prepare *ctx so that switching to it starts entry() on the given stack. */
void ts_context_init(ts_context *ctx, void *stack_top, void (*entry)(void));

#endif
