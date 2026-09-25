#include <stdbool.h>
#include <stdint.h>
#include "header/cpu/gdt.h"
#include "header/cpu/idt.h"
#include "header/cpu/interrupt.h"
#include "header/driver/keyboard.h"
#include "header/kernel-entrypoint.h"
#include "header/text/framebuffer.h"

void kernel_setup(void)
{
    load_gdt(&_gdt_gdtr);
    pic_remap();
    initialize_idt();
    activate_keyboard_interrupt();
    framebuffer_clear();
    framebuffer_set_cursor(0, 0);

    int row = 0, col = 0;
    keyboard_state_activate();
    while (true) {
        char c;
        get_keyboard_buffer(&c);
        if (!c)
            continue;

        if (c == '\n') {
            ++row;
            col = 0;
        } else if (c == '\b') {
            if (col > 0) {
                --col;
            } else if (row > 0) {
                --row;
                col = FRAMEBUFFER_WIDTH - 1;
            }
            framebuffer_write(row, col, ' ', 0xF, 0);
        } else {
            framebuffer_write(row, col, c, 0xF, 0);
            if (++col >= FRAMEBUFFER_WIDTH) {
                ++row;
                col = 0;
            }
        }
        if (row >= FRAMEBUFFER_HEIGHT)
            row = FRAMEBUFFER_HEIGHT - 1;
        framebuffer_set_cursor(row, col);
    }
}
