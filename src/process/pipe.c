#include "header/process/pipe.h"
#include "header/stdlib/string.h"
#include "header/text/framebuffer.h"

static struct {
    char     data[PIPE_BUFFER_SIZE];
    uint32_t len;
    uint32_t owner_pid;
    bool     active;
} pipe_stdin;

static struct {
    char     data[PIPE_BUFFER_SIZE];
    uint32_t len;          // total bytes written, may exceed PIPE_BUFFER_SIZE
    uint32_t owner_pid;
    bool     active;
} pipe_stdout;

static uint32_t min_u32(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

void pipe_set_stdin(uint32_t pid, const char *data, uint32_t len) {
    pipe_stdin.len       = (data == NULL) ? 0 : min_u32(len, PIPE_BUFFER_SIZE);
    pipe_stdin.owner_pid = pid;
    pipe_stdin.active    = true;
    if (pipe_stdin.len > 0)
        memcpy(pipe_stdin.data, data, pipe_stdin.len);
}

void pipe_clear_stdin(void) {
    pipe_stdin.active = false;
    pipe_stdin.len    = 0;
}

int32_t pipe_read_stdin(uint32_t pid, char *buf, uint32_t capacity) {
    if (!pipe_stdin.active || pipe_stdin.owner_pid != pid || buf == NULL)
        return -1;
    uint32_t count = min_u32(pipe_stdin.len, capacity);
    memcpy(buf, pipe_stdin.data, count);
    return (int32_t) count;
}

void pipe_capture_stdout(uint32_t pid) {
    pipe_stdout.len       = 0;
    pipe_stdout.owner_pid = pid;
    pipe_stdout.active    = true;
}

void pipe_stop_capture(void) {
    pipe_stdout.active = false;
}

void pipe_write_stdout(uint32_t pid, const char *s, uint32_t len, uint8_t color) {
    if (!pipe_stdout.active || pipe_stdout.owner_pid != pid) {
        framebuffer_puts(s, len, color);
        return;
    }
    // Keep counting past the end so the reader knows the output was truncated
    for (uint32_t i = 0; i < len; i++) {
        if (pipe_stdout.len < PIPE_BUFFER_SIZE)
            pipe_stdout.data[pipe_stdout.len] = s[i];
        pipe_stdout.len++;
    }
}

uint32_t pipe_read_stdout(char *buf, uint32_t capacity) {
    if (buf == NULL)
        return 0;
    uint32_t count = min_u32(min_u32(pipe_stdout.len, PIPE_BUFFER_SIZE), capacity);
    memcpy(buf, pipe_stdout.data, count);
    return pipe_stdout.len;
}
