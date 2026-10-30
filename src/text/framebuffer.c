#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "header/text/framebuffer.h"
#include "header/stdlib/string.h"
#include "header/cpu/portio.h"

void framebuffer_set_cursor(uint8_t r, uint8_t c) {
    uint16_t pos = r * FRAMEBUFFER_WIDTH + c;
    out(CURSOR_PORT_CMD, 0x0F);
    out(CURSOR_PORT_DATA, (uint8_t) (pos & 0xFF));
    out(CURSOR_PORT_CMD, 0x0E);
    out(CURSOR_PORT_DATA, (uint8_t) ((pos >> 8) & 0xFF));
}

void framebuffer_write(uint8_t row, uint8_t col, char c, uint8_t fg, uint8_t bg) {
    uint16_t index = 2 * (row * FRAMEBUFFER_WIDTH + col);
    FRAMEBUFFER_MEMORY_OFFSET[index]     = (uint8_t) c;
    FRAMEBUFFER_MEMORY_OFFSET[index + 1] = (uint8_t) ((bg << 4) | (fg & 0x0F));
}

void framebuffer_clear(void) {
    for (uint16_t i = 0; i < FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT; i++) {
        FRAMEBUFFER_MEMORY_OFFSET[2 * i]     = 0x00;
        FRAMEBUFFER_MEMORY_OFFSET[2 * i + 1] = 0x07;
    }
}

/* -- Terminal -- */

static uint8_t terminal_row = 0;
static uint8_t terminal_col = 0;

static void framebuffer_scroll(void) {
    uint16_t row_bytes = 2 * FRAMEBUFFER_WIDTH;
    memmove(
        FRAMEBUFFER_MEMORY_OFFSET,
        FRAMEBUFFER_MEMORY_OFFSET + row_bytes,
        row_bytes * (FRAMEBUFFER_HEIGHT - 1)
    );
    for (uint8_t col = 0; col < FRAMEBUFFER_WIDTH; col++)
        framebuffer_write(FRAMEBUFFER_HEIGHT - 1, col, 0, 0x7, 0);
}

static void framebuffer_new_line(void) {
    terminal_col = 0;
    if (terminal_row + 1 < FRAMEBUFFER_HEIGHT)
        terminal_row++;
    else
        framebuffer_scroll();
}

void framebuffer_put_char(char c, uint8_t color) {
    if (c == '\n') {
        framebuffer_new_line();
    } else if (c == '\t') {
        uint8_t spaces = 4 - (terminal_col % 4);
        for (uint8_t i = 0; i < spaces; i++)
            framebuffer_put_char(' ', color);
    } else if (c == '\b') {
        if (terminal_col > 0) {
            terminal_col--;
        } else if (terminal_row > 0) {
            terminal_row--;
            terminal_col = FRAMEBUFFER_WIDTH - 1;
        }
        framebuffer_write(terminal_row, terminal_col, 0, 0x7, 0);
    } else {
        framebuffer_write(terminal_row, terminal_col, c, color, 0);
        if (++terminal_col >= FRAMEBUFFER_WIDTH)
            framebuffer_new_line();
    }
    framebuffer_set_cursor(terminal_row, terminal_col);
}

void framebuffer_puts(const char *str, uint32_t len, uint8_t color) {
    for (uint32_t i = 0; i < len; i++)
        framebuffer_put_char(str[i], color);
}

void framebuffer_reset_terminal(void) {
    framebuffer_clear();
    terminal_row = 0;
    terminal_col = 0;
    framebuffer_set_cursor(0, 0);
}
