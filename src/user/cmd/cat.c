#include "header/user/cmdlib.h"

// cat [file] - print a file, or the pipe input when no file is given
int main(int argc, char **argv) {
    if (argc < 2) {
        int32_t len = read_stdin();
        if (len < 0) {
            print_error("cat", NULL, "missing operand");
            return 1;
        }
        out_puts(stdin_buffer, (uint32_t) len, COLOR_TEXT);
        return 0;
    }

    uint32_t size;
    if (!load_file("cat", argv[1], &size))
        return 1;
    out_puts((char*) file_buffer, size, COLOR_TEXT);
    if (size > 0 && file_buffer[size - 1] != '\n')
        out_putchar('\n', COLOR_TEXT);
    return 0;
}
