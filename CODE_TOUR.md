# Code Tour

Read in this order. Each section: what the file does, the decision that matters, and the sentence to say if an interviewer points at it.

## 1. `include/timeslice.h` — the API

Start here: it is short and it names everything. Three things to notice:

- **Two clocks.** `TS_VIRTUAL` advances only through `ts_work()`, so runs are deterministic and can be compared with a textbook. `TS_REAL` is a timer signal that interrupts running code.
- **Priorities follow textbooks:** a smaller number is more important.
- **`ts_queue` is intrusive.** Mutexes, condition variables and semaphores hold a head and tail pointer, and the link lives in the thread itself (`next`). A thread waits in one queue at a time, so no queue ever allocates. This is how kernels link their run queues.

## 2. `src/context.h` and `src/context.S` — the context switch

`ts_context_switch(from, to)` stores the current registers in `*from`, loads `*to`, and returns, into the other thread.

The registers saved are only the **callee-saved** ones. The compiler treats `ts_context_switch` like any function call, so before calling it the caller has already saved every caller-saved (scratch) register it still needs. What a function must preserve for its caller is exactly what must survive a switch:

- **arm64:** x19-x28, x29 (frame pointer), x30 (link register, the return address), sp, and d8-d15. Note x18 is untouched: on Apple platforms it is reserved.
- **x86-64:** rbx, rbp, r12-r15, rsp, plus the MXCSR and x87 control words. There is no link register on x86, so the return address is read off the top of the stack and the saved rsp is the stack as it will be after returning.

The last instruction is `ret` (arm64) or `jmp *` (x86-64). That is where control lands in the other thread: wherever it last called `ts_context_switch`.

> *"A context switch only needs the callee-saved registers, because the calling convention already made the caller save the rest. On arm64 that's 21 words."*

## 3. `src/context.c` — a thread's first context

A new thread has never called `ts_context_switch`, so its first context is built by hand: the stack pointer at the top of its stack (16-byte aligned, which both ABIs require), and the "return address" set to `thread_entry`. The first switch to it therefore "returns" into `thread_entry`. On x86-64 the stack pointer is set 8 below an aligned address, as if a `call` had just pushed a return address, because that is what a function expects at entry.

## 4. `src/internal.h` — the thread and the scheduler state

`struct ts_thread` holds the saved context, the stack, the state (`NEW`, `READY`, `RUNNING`, `BLOCKED`, `SLEEPING`, `DONE`), the accounting (`used`, `slice_used`, MLFQ's `level` and `level_used`), and two priorities: `base_prio` (its own) and `prio` (effective, possibly borrowed through inheritance). `ts_sched S` is the global scheduler state; there is one CPU.

`used` is `volatile`: on the real clock the signal handler increments it while `ts_work` spins reading it.

## 5. `src/sched.c` — the scheduler

Read it top to bottom; the sections are marked.

**The loop, `ts_run()`.** Admit threads that have arrived or woken, take the best ready thread, `dispatch()` it, repeat. `dispatch()` switches from the loop's context to the thread's, and returns when the thread yields, blocks, finishes or is preempted. Threads never switch straight to each other; everything goes through the loop, so every decision is made in one place. When nothing is ready, the loop skips the clock to the next arrival or wake-up (virtual) or waits for the timer with `sigsuspend` (real). If nothing is ready and nothing can ever become ready, it is a **deadlock**, and `ts_run` reports which threads are blocked.

**The policies.** `peek_best()` returns who would run next: FCFS and round robin take the queue in order; SJF, SRTF and priority scan it with `better()`; MLFQ takes the head of the highest non-empty level. `should_preempt()` answers whether the running thread must give way: SRTF to a strictly shorter job, preemptive priority to a strictly more important one, round robin when its quantum is used and someone is waiting, MLFQ when a higher level has work or it has used up its allotment.

**The clock.** `tick()` accounts for one tick and admits arrivals. `scheduling_point()` makes the decision. `ts_work()` on the virtual clock calls `scheduling_point()` *before* each tick, not after, and the long comment above `scheduling_point` is why: deciding after a thread's last tick preempts a thread with nothing left to do, and its completion time comes out late. The agreement test found this.

**The real clock.** `on_tick()` is the `SIGALRM` handler: it accounts a tick for the running thread and may preempt it right there, switching away from inside the handler. The thread resumes inside the handler later, returns from it, and `sigreturn` restores its registers and its signal mask.

**Critical sections.** `enter_critical()` / `leave_critical()` mask and unmask `SIGALRM`. This is the interrupt discipline of a kernel or an RTOS (`taskENTER_CRITICAL` in FreeRTOS): scheduler code runs with the timer masked, and every path back into a thread unmasks it. `ts_yield()` shows the pattern: mask, queue itself, switch away, and on the way back, unmask.

> *"The timer signal is my interrupt. The scheduler runs with it masked, and every way back into a thread unmasks it: sigreturn for a thread preempted in the handler, an explicit unmask after a yield, and the entry function for a new thread. If you miss one, preemption stops after the first switch; there's a test for that."*

**Stacks.** `make_stack()` maps the stack plus one page and makes the lowest page inaccessible. Stacks grow down, so an overflow touches that page and faults immediately.

## 6. `src/sync.c` — mutexes, condition variables, semaphores

**Handoff.** `ts_mutex_unlock` gives the mutex directly to the next waiter and makes it ready. The woken thread never has to race for it, so `ts_mutex_lock` needs no retry loop.

**Priority inheritance.** `lend_priority()` raises the owner to the waiter's priority, and follows the chain if the owner is itself waiting on an inheriting mutex. `recompute_priority()` runs on unlock: a thread's priority is its own, or the most important priority among the waiters on the inheriting mutexes it still holds. This is the fix for the **Mars Pathfinder** bug (1997): a low-priority task held a lock a high-priority task needed, a medium-priority task kept the low one off the CPU, and a watchdog kept resetting the spacecraft.

**Condition variables** release the mutex and block in one critical section, so a signal cannot slip in between and be lost. They are Mesa-style, like pthreads: a wake-up is a hint, so waiters re-check in a `while` loop.

> *"Priority inheritance: while a high-priority thread waits for my lock, I run at its priority, so a medium thread can't hold me off the CPU. My test reproduces the inversion: without it the high thread finishes at tick 12, behind a thread that never touches the lock; with it, at tick 6."*

## 7. `src/textbook.c` — the independent answer

A tick-by-tick calculation with arrays, the way an OS course asks for it. It shares no code with the scheduler on purpose: the tests run both and demand identical timelines, so each checks the other. Its tie-break rules are the textbook ones, and the scheduler's `better()` and `tick()` are written to match.

## 8. `tools/tsexam.c` — the exam tool

Reads a process table (or generates one with `--practice`), runs the textbook calculation and the scheduler, and prints both Gantt charts and the per-process table. It is the useful end of the project: a way to check OS exam answers against a real run.

## 9. `tests/`

- `check.c` / `check.h`: the harness. `run_isolated()` forks, so the real-clock and crash tests run in a child with a deadline.
- `test_textbook.c`: the Silberschatz worked examples, plus the tie-break conventions.
- `test_agree.c`: 27,000 random workloads, scheduler against textbook.
- `test_context.c`: a million switches; the guard page (caught by a handler on `sigaltstack`, because the overflowed stack cannot run a handler).
- `test_sync.c`: the lost-update race, priority inversion, the bounded buffer, the semaphore, deadlock, trylock.
- `test_mlfq.c`: sinking, short jobs first, anti-gaming, the boost.
- `test_real.c`: preemption of code that never yields.

## The 60-second walkthrough

> *"Timeslice is a user-space threading library in C. Each thread has its own mmapped stack with a guard page, and I switch between them in assembly, saving only the callee-saved registers, about 7 nanoseconds a switch. A scheduler loop picks who runs: FCFS, SJF, SRTF, priority, round robin, or MLFQ. On the real clock a timer signal preempts code that never yields, and the scheduler runs with that signal masked, like a kernel with interrupts off. The part I'd point at is the test: a separate textbook calculation, and the real scheduler must produce the same Gantt chart on 27,000 random workloads. The first run failed on three of them, all one bug in when the scheduler made its decision."*

## The three stories

**1. The bug the textbook caught.** The first agreement run failed on three workloads, and the Gantt charts printed identically. The difference was in completion times: a thread preempted on the exact tick its work ended was put back in the queue with nothing left to do, and only "finished" when it next got the CPU, a turn zero ticks long, so invisible in the chart. The fix was to make the scheduling decision when a thread asks for its next tick, not after its last.

**2. Preemption is an interrupt.** Switching threads inside a signal handler means the timer must be masked in scheduler code and unmasked on every path back into a thread. Drop the unmask in `ts_yield` and preemption dies after the first yield; the test runs in a child with a deadline so that failure shows up as a failure, not a hang.

**3. Mars Pathfinder.** Priority inversion, reproduced tick for tick, and fixed with inheritance. It is the classic embedded-systems bug, and it is in the tests.
