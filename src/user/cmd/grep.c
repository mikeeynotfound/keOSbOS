#include "header/user/cmdlib.h"
#include "header/stdlib/string.h"

// Print every line of text[0..size) that matches the regex subset
static void grep_text(const char *text, uint32_t size, const char *pat, uint32_t pat_len) {
    uint32_t start = 0;
    while (start < size) {
        uint32_t end = start;
        while (end < size && text[end] != '\n')
            end++;
        if (regex_match(text + start, end - start, pat, pat_len)) {
            out_puts(text + start, end - start, COLOR_TEXT);
            out_putchar('\n', COLOR_TEXT);
        }
        start = end + 1;
    }
}

// grep <regex> [file] - print matching lines, from a file or the pipe input
int main(int argc, char **argv) {
    if (argc < 2) {
        print_error("grep", NULL, "usage: grep <regex> [file]");
        return 1;
    }
    const char *pat     = argv[1];
    uint32_t    pat_len = str_len(pat);

    if (!regex_valid(pat, pat_len)) {
        print_error("grep", pat, "invalid regex");
        return 1;
    }

    if (argc >= 3) {
        uint32_t size;
        if (!load_file("grep", argv[2], &size))
            return 1;
        grep_text((char*) file_buffer, size, pat, pat_len);
        return 0;
    }

    int32_t len = read_stdin();
    if (len < 0) {
        print_error("grep", NULL, "missing file or pipe input");
        return 1;
    }
    grep_text(stdin_buffer, (uint32_t) len, pat, pat_len);
    return 0;
}
