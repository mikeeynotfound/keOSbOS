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
