#include "header/cpu/interrupt.h"
#include "header/cpu/gdt.h"
#include "header/cpu/portio.h"
#include "header/driver/keyboard.h"
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

void main_interrupt_handler(struct InterruptFrame frame) {
    switch (frame.int_number) {
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
        default:
            break;
    }
}
