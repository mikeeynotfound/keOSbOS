#include "header/cpu/interrupt.h"
#include "header/cpu/gdt.h"
#include "header/cpu/portio.h"
#include "header/driver/keyboard.h"
#include "header/driver/cmos.h"
#include "header/process/scheduler.h"
#include "header/filesystem/ext2.h"
#include "header/text/framebuffer.h"
#include "header/syscall-number.h"

/**
 * Single TSS for the system. ss0 is the kernel data segment used whenever the
 * CPU switches from ring 3 to ring 0 (interrupts and system calls).
 */
struct TSSEntry _interrupt_tss_entry = {
    .ss0 = GDT_KERNEL_DATA_SEGMENT_SELECTOR,
};

void set_tss_kernel_current_stack(void) {
    uint32_t stack_ptr;
    // Read the base stack frame instead of esp so the saved pointer remains
    // valid after this function returns.
    __asm__ volatile("mov %%ebp, %0" : "=r"(stack_ptr) : /* <Empty> */);
    // +4 for the return address and +4 for stack_ptr itself.
    _interrupt_tss_entry.esp0 = stack_ptr + 8;
}

void io_wait(void) {
    out(0x80, 0);
}

void pic_ack(uint8_t irq) {
    if (irq >= 8)
        out(PIC2_COMMAND, PIC_ACK);
    out(PIC1_COMMAND, PIC_ACK);
}

void pic_remap(void) {
    // Starts the initialization sequence in cascade mode
    out(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    out(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    out(PIC1_DATA, PIC1_OFFSET); // ICW2: Master PIC vector offset
    io_wait();
    out(PIC2_DATA, PIC2_OFFSET); // ICW2: Slave PIC vector offset
    io_wait();
    out(PIC1_DATA, 0b0100); // ICW3: tell Master PIC, slave PIC at IRQ2 (0000 0100)
    io_wait();
    out(PIC2_DATA, 0b0010); // ICW3: tell Slave PIC its cascade identity (0000 0010)
    io_wait();

    out(PIC1_DATA, ICW4_8086);
    io_wait();
    out(PIC2_DATA, ICW4_8086);
    io_wait();

    // Disable all interrupts
    out(PIC1_DATA, PIC_DISABLE_ALL_MASK);
    out(PIC2_DATA, PIC_DISABLE_ALL_MASK);
}

void activate_keyboard_interrupt(void) {
    out(PIC1_DATA, in(PIC1_DATA) & ~(1 << IRQ_KEYBOARD));
}

void activate_timer_interrupt(void) {
    __asm__ volatile("cli");

    // Channel 0, square wave, LOBYTE/HOBYTE access, binary counter.
    out(PIT_COMMAND_REGISTER_PIO, PIT_COMMAND_VALUE);
    out(PIT_CHANNEL_0_DATA_PIO, (uint8_t) (PIT_TIMER_COUNTER & 0xFF));
    out(PIT_CHANNEL_0_DATA_PIO, (uint8_t) ((PIT_TIMER_COUNTER >> 8) & 0xFF));

    // Unmask IRQ0. Interrupts are re-enabled by the iret that enters user mode.
    out(PIC1_DATA, in(PIC1_DATA) & ~(1 << IRQ_TIMER));
}

static bool is_from_user_mode(struct InterruptFrame *frame) {
    return (frame->int_stack.cs & 0x3) == 0x3;
}

void main_interrupt_handler(struct InterruptFrame frame) {
    // CPU exception (vector 0-31). Returning would re-run the faulting
    // instruction forever, so stop the faulty process instead.
    if (frame.int_number < PIC1_OFFSET) {
        struct ProcessControlBlock *pcb = process_get_current_running_pcb_pointer();
        if (is_from_user_mode(&frame) && pcb != NULL) {
            static const char msg[] = "\nkernel: process killed by CPU exception\n";
            framebuffer_puts(msg, sizeof(msg) - 1, 0xC);
            process_destroy(pcb->metadata.pid);
            scheduler_switch_to_next_process();
        }
        static const char panic[] = "\nKERNEL PANIC: CPU exception in kernel\n";
        framebuffer_puts(panic, sizeof(panic) - 1, 0x4F);
        while (true)
            __asm__ volatile("cli; hlt");
    }

    switch (frame.int_number) {
        case PIC1_OFFSET + IRQ_TIMER:
            pic_ack(IRQ_TIMER);
            // The kernel itself is not preemptible; only user processes are.
            if (is_from_user_mode(&frame) && process_get_current_running_pcb_pointer() != NULL)
                scheduler_preempt(frame);
            break;
        case PIC1_OFFSET + IRQ_KEYBOARD:
            keyboard_isr();
            break;
        case SYSCALL_INT_VECTOR:
            syscall(frame);
            break;
        default:
            break;
    }
}

void syscall(struct InterruptFrame frame) {
    uint32_t ebx = frame.cpu.general.ebx;
    uint32_t ecx = frame.cpu.general.ecx;
    uint32_t edx = frame.cpu.general.edx;

    switch (frame.cpu.general.eax) {
        case SYSCALL_READ:
            *((int8_t*) ecx) = read(*(struct EXT2DriverRequest*) ebx);
            break;
        case SYSCALL_READ_DIRECTORY:
            *((int8_t*) ecx) = read_directory((struct EXT2DriverRequest*) ebx);
            break;
        case SYSCALL_WRITE:
            *((int8_t*) ecx) = write((struct EXT2DriverRequest*) ebx);
            break;
        case SYSCALL_DELETE:
            *((int8_t*) ecx) = delete(*(struct EXT2DriverRequest*) ebx);
            break;
        case SYSCALL_GETCHAR:
            get_keyboard_buffer((char*) ebx);
            break;
        case SYSCALL_PUTCHAR:
            framebuffer_put_char((char) ebx, (uint8_t) ecx);
            break;
        case SYSCALL_PUTS:
            framebuffer_puts((const char*) ebx, ecx, (uint8_t) edx);
            break;
        case SYSCALL_KEYBOARD_ON:
            keyboard_state_activate();
            break;
        case SYSCALL_CLEAR_SCREEN:
            framebuffer_reset_terminal();
            break;
        case SYSCALL_EXEC:
            *((int32_t*) ecx) = process_create_user_process(*(struct EXT2DriverRequest*) ebx);
            break;
        case SYSCALL_PS:
            *((uint32_t*) edx) = process_get_info_list((struct ProcessInfo*) ebx, ecx);
            break;
        case SYSCALL_KILL: {
            struct ProcessControlBlock *current = process_get_current_running_pcb_pointer();
            struct ProcessControlBlock *target  = process_get_pcb_by_pid(ebx);
            if (target == NULL) {
                *((int8_t*) ecx) = PROCESS_KILL_NOT_FOUND;
            } else if (target->metadata.pid == 0) {
                *((int8_t*) ecx) = PROCESS_KILL_NOT_PERMITTED;
            } else {
                // Check before destroy: afterwards the PCB is cleared and
                // nothing is RUNNING, so the old comparison was always false.
                bool is_self = (target == current);
                *((int8_t*) ecx) = PROCESS_KILL_SUCCESS;
                process_destroy(ebx);
                if (is_self)
                    scheduler_switch_to_next_process();
            }
            break;
        }
        case SYSCALL_EXIT: {
            struct ProcessControlBlock *current = process_get_current_running_pcb_pointer();
            if (current != NULL) {
                process_destroy(current->metadata.pid);
                scheduler_switch_to_next_process();
            }
            break;
        }
        case SYSCALL_GET_TIME:
            cmos_read_rtc((struct RTCTime*) ebx);
            break;
        case SYSCALL_PUTCHAR_AT:
            framebuffer_write((edx >> 8) & 0xFF, edx & 0xFF, (char) ebx, (uint8_t) ecx, 0);
            break;
        case SYSCALL_YIELD:
            if (process_get_current_running_pcb_pointer() != NULL)
                scheduler_preempt(frame);
            break;
        case SYSCALL_GET_PID: {
            struct ProcessControlBlock *current = process_get_current_running_pcb_pointer();
            *((uint32_t*) ebx) = current != NULL ? current->metadata.pid : 0;
            break;
        }
        default:
            break;
    }
}
