#ifndef _SCHEDULER_H
#define _SCHEDULER_H

#include "header/process/process.h"

/**
 * Read all general purpose register values and set control register.
 * Resume the execution flow back to ctx.eip and ctx.eflags
 *
 * @note          Implemented in assembly (process/context-switch.s)
 * @param context Target context to switch into
 */
__attribute__((noreturn)) extern void process_context_switch(struct Context ctx);

/* --- Scheduler --- */
/**
 * Initialize scheduler before executing init process
 */
void scheduler_init(void);

/**
 * Save context to current running process
 *
 * @param ctx Context to save to current running process control block
 */
void scheduler_save_context_to_current_running_pcb(struct Context ctx);

/**
 * Trigger the scheduler algorithm (round robin) and context switch to new process
 */
__attribute__((noreturn)) void scheduler_switch_to_next_process(void);

/**
 * Save the interrupted user context and switch to the next process.
 * Used by the timer interrupt (preemption) and SYSCALL_YIELD.
 *
 * @param frame Interrupt frame of the interrupted process
 */
__attribute__((noreturn)) void scheduler_preempt(struct InterruptFrame frame);

#endif
