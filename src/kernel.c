#include <stdbool.h>
#include <stdint.h>
#include "header/cpu/gdt.h"
#include "header/cpu/idt.h"
#include "header/cpu/interrupt.h"
#include "header/driver/keyboard.h"
#include "header/filesystem/ext2.h"
#include "header/kernel-entrypoint.h"
#include "header/memory/paging.h"
#include "header/stdlib/string.h"
#include "header/text/framebuffer.h"

// User program is a flat binary linked at virtual address 0 (see user-linker.ld)
#define USER_PROGRAM_BASE         0x0u
#define USER_PROGRAM_MAX_SIZE     0x40000u    // 256 KiB mapped for code/data
#define USER_STACK_PAGE_COUNT     4u          // 16 KiB stack right below 4 MiB
#define USER_STACK_TOP            0x400000u   // esp = base + 4 MiB - 4
#define USER_PROGRAM_PARENT_INODE 1u          // root directory inode of this EXT2

// Staging buffer: ext2 read() rejects a NULL buffer, so it cannot read straight into address 0
static uint8_t user_program_buffer[USER_PROGRAM_MAX_SIZE];

static bool map_user_pages(uint32_t start_addr, uint32_t size) {
    for (uint32_t offset = 0; offset < size; offset += PAGE_FRAME_SIZE) {
        if (!paging_allocate_user_page_frame(&_paging_kernel_page_directory, (void *) (start_addr + offset)))
            return false;
    }
    return true;
}

static bool load_user_program(char *name, uint8_t name_len) {
    uint32_t stack_size = USER_STACK_PAGE_COUNT * PAGE_FRAME_SIZE;
    if (!paging_allocate_check(USER_PROGRAM_MAX_SIZE + stack_size))
        return false;

    struct EXT2DriverRequest request = {
        .buf          = user_program_buffer,
        .name         = name,
        .name_len     = name_len,
        .parent_inode = USER_PROGRAM_PARENT_INODE,
        .buffer_size  = sizeof(user_program_buffer),
        .is_directory = false,
    };
    if (read(request) != 0)
        return false;

    if (!map_user_pages(USER_PROGRAM_BASE, USER_PROGRAM_MAX_SIZE) ||
        !map_user_pages(USER_STACK_TOP - stack_size, stack_size))
        return false;

    memcpy((void *) USER_PROGRAM_BASE, user_program_buffer, sizeof(user_program_buffer));
    return true;
}

void kernel_setup(void)
{
    load_gdt(&_gdt_gdtr);
    pic_remap();
    initialize_idt();
    activate_keyboard_interrupt();
    framebuffer_clear();
    framebuffer_set_cursor(0, 0);
    initialize_filesystem_ext2();

    // Prepare ring 3 support. The TSS stays installed for later user mode
    // entry (and is used by inter-privilege interrupts once user mode runs).
    gdt_install_tss();
    set_tss_register();

    keyboard_state_activate();

    if (load_user_program("shell", 5)) {
        set_tss_kernel_current_stack();
        kernel_execute_user_program((void *) USER_PROGRAM_BASE);
    }

    int row = 0, col = 0;
    while (true) {
        char c;
        get_keyboard_buffer(&c);
        if (!c)
            continue;

        if (c == '\n') {
            ++row;
            col = 0;
        }
        else if (c == '\t') {
            col += 4 - (col % 4);
            if (col >= FRAMEBUFFER_WIDTH) {
                ++row;
                col = 0;
            }
        }
        else if (c == '\b') {
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
