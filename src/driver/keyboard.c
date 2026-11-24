#include "header/driver/keyboard.h"
#include "header/cpu/portio.h"
#include "header/stdlib/string.h"

const char keyboard_scancode_1_to_ascii_map[256] = {
      0, 0x1B, '1', '2', '3', '4', '5', '6',  '7', '8', '9',  '0',  '-', '=', '\b', '\t',
    'q',  'w', 'e', 'r', 't', 'y', 'u', 'i',  'o', 'p', '[',  ']', '\n',   0,  'a',  's',
    'd',  'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',   0, '\\',  'z', 'x',  'c',  'v',
    'b',  'n', 'm', ',', '.', '/',   0, '*',    0, ' ',   0,    0,    0,   0,    0,    0,
      0,    0,   0,   0,   0,   0,   0,   0,    0,   0, '-',    0,    0,   0,  '+',    0,
      0,    0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,
      0,    0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,
      0,    0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,

      0,    0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,
      0,    0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,
      0,    0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,
      0,    0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,
      0,    0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,
      0,    0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,
      0,    0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,
      0,    0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,
};

const char keyboard_scancode_1_to_ascii_shift_map[256] = {
      0, 0x1B, '!', '@', '#', '$', '%', '^',  '&', '*', '(',  ')',  '_', '+', '\b', '\t',
    'Q',  'W', 'E', 'R', 'T', 'Y', 'U', 'I',  'O', 'P', '{',  '}', '\n',   0,  'A',  'S',
    'D',  'F', 'G', 'H', 'J', 'K', 'L', ':',  '"', '~',   0,  '|',  'Z', 'X',  'C',  'V',
    'B',  'N', 'M', '<', '>', '?',   0, '*',    0, ' ',   0,    0,    0,   0,    0,    0,
      0,    0,   0,   0,   0,   0,   0,   0,    0,   0, '-',    0,    0,   0,  '+',    0,
};

static struct KeyboardDriverState keyboard_state = {
    .read_extended_mode = false,
    .keyboard_input_on  = false,
    .keyboard_buffer    = 0,
    .shift_on           = false,
    .caps_lock_on       = false,
};

static char scancode_to_ascii(uint8_t scancode) {
    char c = keyboard_state.shift_on
        ? keyboard_scancode_1_to_ascii_shift_map[scancode]
        : keyboard_scancode_1_to_ascii_map[scancode];

    // Caps lock only affects letters, and is inverted while shift is held
    if (keyboard_state.caps_lock_on) {
        if ('a' <= c && c <= 'z')
            c = c - 'a' + 'A';
        else if ('A' <= c && c <= 'Z')
            c = c - 'A' + 'a';
    }
    return c;
}

void keyboard_state_activate(void) {
    keyboard_state.keyboard_buffer   = 0;
    keyboard_state.keyboard_input_on = true;
}

void keyboard_state_deactivate(void) {
    keyboard_state.keyboard_input_on = false;
}

void get_keyboard_buffer(char *buf) {
    *buf = keyboard_state.keyboard_buffer;
    keyboard_state.keyboard_buffer = 0;
}

void keyboard_isr(void) {
    uint8_t scancode = in(KEYBOARD_DATA_PORT);

    if (scancode == EXTENDED_SCANCODE_BYTE) {
        keyboard_state.read_extended_mode = true;
    } else if (keyboard_state.read_extended_mode) {
        // Extended keys (arrows, right ctrl, etc) are not mapped to ASCII, skip them
        keyboard_state.read_extended_mode = false;
    } else {
        bool    is_break = scancode & SCANCODE_BREAK_BIT;
        uint8_t make     = scancode & ~SCANCODE_BREAK_BIT;

        if (make == SCANCODE_LEFT_SHIFT || make == SCANCODE_RIGHT_SHIFT) {
            keyboard_state.shift_on = !is_break;
        } else if (!is_break && make == SCANCODE_CAPS_LOCK) {
            keyboard_state.caps_lock_on = !keyboard_state.caps_lock_on;
        } else if (!is_break && keyboard_state.keyboard_input_on) {
            char c = scancode_to_ascii(make);
            if (c)
                keyboard_state.keyboard_buffer = c;
        }
    }

    pic_ack(IRQ_KEYBOARD);
}
