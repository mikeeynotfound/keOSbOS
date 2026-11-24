#include "header/process/scheduler.h"
#include "header/text/framebuffer.h"

// Index in _process_list of the last scheduled process, -1 before the first switch
static int32_t scheduler_last_index = -1;

void scheduler_init(void) {
    scheduler_last_index = -1;
    activate_timer_interrupt();
}

void scheduler_save_context_to_current_running_pcb(struct Context ctx) {
    struct ProcessControlBlock *pcb = process_get_current_running_pcb_pointer();
    if (pcb != NULL)
        pcb->context = ctx;
}

__attribute__((noreturn)) void scheduler_switch_to_next_process(void) {
    struct ProcessControlBlock *current = process_get_current_running_pcb_pointer();
    if (current != NULL)
        current->metadata.state = PROCESS_STATE_READY;

    // Round robin: first READY process after the last scheduled one (wrapping around)
    int32_t start = scheduler_last_index;
    for (int32_t step = 1; step <= PROCESS_COUNT_MAX; step++) {
        int32_t idx = (start + step + PROCESS_COUNT_MAX) % PROCESS_COUNT_MAX;
        struct ProcessControlBlock *next = &_process_list[idx];
        if (next->metadata.state != PROCESS_STATE_READY)
            continue;

        scheduler_last_index = idx;
        next->metadata.state = PROCESS_STATE_RUNNING;
        process_context_switch(next->context);
    }

    static const char no_process_msg[] = "\nkernel: no process left, system halted\n";
    framebuffer_puts(no_process_msg, sizeof(no_process_msg) - 1, 0xC);
    while (true)
        __asm__ volatile("cli; hlt");
}

__attribute__((noreturn)) void scheduler_preempt(struct InterruptFrame frame) {
    struct Context ctx = {
        .cpu    = frame.cpu,
        .eip    = frame.int_stack.eip,
        .eflags = frame.int_stack.eflags,
        .page_directory_virtual_addr = paging_get_current_page_directory_addr(),
    };

    scheduler_save_context_to_current_running_pcb(ctx);
    scheduler_switch_to_next_process();
}
