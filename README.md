# Timeslice

User-space threads in C with real schedulers: FCFS, SJF, SRTF, priority (with and without preemption), round robin, and a multi-level feedback queue. The threads have their own stacks, the library switches between them with a few lines of assembly, and on the real clock a timer signal preempts code that never yields, the way a kernel or an RTOS does.

It began as the textbook exercise every OS course sets, working out FCFS and round-robin Gantt charts, and asks a stricter question: if the scheduler is real, running real threads, does it produce the textbook's answer? It does, and a test holds it to that on 27,000 random workloads.

## What is in it

- **Context switching** in assembly for arm64 and x86-64: only the callee-saved registers, because the calling convention guarantees the rest are already saved by the caller. About 7 ns per switch on an M4 Pro.
- **Stacks with guard pages.** Each thread's stack is `mmap`ed with one inaccessible page below it, so an overflow faults at once instead of silently overwriting other memory.
- **Seven policies**, one scheduler loop. The choosing policies (SJF, SRTF, priority) scan the ready queue, which keeps priority inheritance simple: a raised priority takes effect without reordering anything.
- **Two clocks.** The *virtual* clock advances only when a thread calls `ts_work()`, one tick at a time, which makes every run deterministic. The *real* clock is a `setitimer` signal that preempts whatever is running.
- **Mutexes with priority inheritance**, condition variables, semaphores, and deadlock detection.
- **`tsexam`**, a command-line tool for OS exam questions: it prints the Gantt chart and completion, turnaround, waiting and response times from both the textbook calculation and a real run, generates practice questions, and says whether the two agree.

## tsexam

```
$ cat srtf.txt
# name arrival burst
P1 0 8
P2 1 4
P3 2 9
P4 3 5

$ tsexam --policy srtf srtf.txt
Gantt chart (textbook calculation)
  |P1  |P2      |P4        |P1            |P3                |
  0    1        5          10             17                 26
Gantt chart (the scheduler, running real threads)
  |P1  |P2      |P4        |P1            |P3                |
  0    1        5          10             17                 26

Process  Completion  Turnaround  Waiting  Response
P1               17          17        9         0
P2                5           4        0         0
P3               26          24       15        15
P4               10           7        2         2
Average                    13.00     6.50      4.25

The scheduler matches the textbook answer (5 context switches, 1 preemption).
```

`tsexam --practice --policy rr --quantum 2 --seed 7` prints a random question to solve by hand; add `--answer` to check it.

## Using the library

```c
#include "timeslice.h"

static void worker(void *arg) { ts_work(*(unsigned *)arg); }

int main(void) {
    static unsigned bursts[] = {24, 3, 3};
    ts_init(&(ts_config){.policy = TS_RR, .quantum = 4});
    for (int i = 0; i < 3; i++) ts_spawn(worker, &bursts[i], NULL);
    ts_run();

    ts_stats st[3];
    ts_stats_get(st, 3);  /* completion, turnaround, waiting, response, preemptions */
    ts_shutdown();
}
```

## The design decisions

**A context switch saves only callee-saved registers.** On arm64 that is x19-x28, the frame pointer, the link register, the stack pointer and d8-d15: 21 words. The caller of `ts_context_switch` has already saved anything else it needs, because as far as the compiler knows it is calling an ordinary function. On x86-64 it is rbx, rbp, r12-r15, rsp, the resume address, and the MXCSR and x87 control words, since rounding modes are callee-saved too.

**The timer signal is treated as an interrupt.** Scheduler code runs with `SIGALRM` masked, as a kernel runs with interrupts off, and every path back into a thread unmasks it: a thread preempted inside the handler gets its mask back from `sigreturn`, a thread returning from `ts_yield` unmasks explicitly, and a new thread unmasks in its entry function. Miss one path and preemption silently stops after the first switch; there is a test for exactly that.

**Scheduling decisions happen when a thread asks for its next tick.** Deciding straight after a tick instead would preempt a thread on the very tick its work ends, and it would then be recorded as finishing only when it next got the CPU. The Gantt chart looks identical, because that last turn is zero ticks long; only the completion time is wrong. The comparison with the textbook calculation caught this. See "Tests".

**Tie-breaks follow the textbook convention**, so answers match what an exam expects: equal keys go to the earlier arrival, then to the lower process number; a process arriving on the tick a round-robin quantum expires queues ahead of the process being preempted; and in SRTF an arrival with equal remaining time does not preempt.

**MLFQ follows the rules in *Operating Systems: Three Easy Pieces***: new threads start at the top; a thread that uses up its allotment at a level moves down, however many times it gave up the CPU along the way, so yielding just before the quantum ends does not keep it at the top; and an optional periodic boost returns everyone to the top, so a CPU-bound thread is not starved.

**Priority inheritance follows the chain.** While a high-priority thread waits for a mutex, its owner runs at the high priority, and if that owner is itself waiting on another inheriting mutex, that mutex's owner is raised too. Without it, the test reproduces the Mars Pathfinder inversion: the high-priority thread finishes at tick 12, stuck behind a medium-priority thread that never touches the lock. With it, it finishes at tick 6.

## Tests

30 tests, and they pass under UndefinedBehaviorSanitizer (`-DTS_SANITIZE=undefined`).

- **The textbook examples** from Silberschatz, *Operating System Concepts*, chapter 5: FCFS (average wait 17), SJF (7), SRTF (6.5), round robin with q=4 (17/3) and non-preemptive priority (8.2), each through both the textbook calculation and the scheduler.
- **Agreement**: 27,000 random process tables across FCFS, SJF, SRTF, both priority policies and round robin with four quanta. The scheduler and `src/textbook.c`, a deliberately separate array-based calculation, must produce the same Gantt chart and the same completion and first-run times for every process.
- **Context switching**: a million switches with values held in callee-saved registers on both sides, and a stack overflow that must end in the guard page. The overflow is caught by a handler on an alternate signal stack, since the thread's own stack is the thing that ran out.
- **Synchronisation**: a counter that loses updates without the mutex (even on one CPU, preemption is enough for a race) and none with it; priority inversion with and without inheritance; a bounded buffer on two condition variables; a semaphore admitting exactly three; and a deadlock that `ts_run` detects and reports.
- **MLFQ**: a CPU-bound thread sinks; a new short job runs ahead of it at once where FCFS makes it wait 20 ticks; yielding does not game it; and a stream of new jobs starves the CPU-bound thread without a boost but not with one.
- **The real clock**: two busy loops that never yield still take turns; preemption keeps working after a thread has yielded; a sleeping thread wakes on time. These run in a child process with a deadline, so if preemption breaks the test fails instead of hanging.

The first run of the agreement test failed on three workloads, all the same bug: the zero-length last turn described above. And the tests were checked for being able to fail. Five bugs were planted one at a time, and each was caught by the test written for it: `ts_yield` not unmasking the timer, the context switch dropping x19 and x20, priority inheritance switched off, MLFQ resetting a thread's allotment on every slice, and deciding right after a tick again.

## Limits

- **Tested on arm64 macOS.** The x86-64 build passes everything on the virtual clock under Rosetta 2, context switches included. Real-clock preemption does not: under Rosetta it dies with `SIGILL`. A single switch away and back inside one signal handler works under Rosetta; what fails is resuming a different thread that was parked inside an earlier handler, so that handlers return in a different order from the one their signals arrived in. It has not been run on native x86-64. The robust fix is the one Go uses for asynchronous preemption: have the handler redirect the interrupted thread into a function that yields, instead of switching stacks inside the handler.
- **One OS thread.** Everything runs on the thread that calls `ts_run()`. It is a model of a single CPU, not a replacement for pthreads.
- **Picking is O(n)** in the number of ready threads for the choosing policies. A heap would make it O(log n); for the sizes this is used at, the scan is simpler and fast.

## Build

```
cmake -S . -B build && cmake --build build -j
./build/timeslice_tests
./build/tsexam --help
```

## Layout

```
include/timeslice.h   the public API
include/textbook.h
src/context.S         the context switch, arm64 and x86-64
src/context.c         setting up a new thread's first context
src/sched.c           threads, the clocks, dispatching, the policies, results
src/sync.c            mutexes with priority inheritance, condition variables, semaphores
src/textbook.c        the independent textbook calculation
tools/tsexam.c        the exam tool
tests/                the test suite and its harness
```
