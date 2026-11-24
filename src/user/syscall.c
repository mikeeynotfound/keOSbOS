#include "header/user/syscall.h"

void syscall(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx) {
    // The kernel restores every register on return, only memory may change
    __asm__ volatile("int $0x30" : : "a"(eax), "b"(ebx), "c"(ecx), "d"(edx) : "memory");
}

static int8_t fs_syscall(uint32_t number, struct EXT2DriverRequest *request) {
    int8_t retcode = -1;
    syscall(number, (uint32_t) request, (uint32_t) &retcode, 0);
    return retcode;
}

int8_t sys_read(struct EXT2DriverRequest *request) {
    return fs_syscall(SYSCALL_READ, request);
}

int8_t sys_read_directory(struct EXT2DriverRequest *request) {
    return fs_syscall(SYSCALL_READ_DIRECTORY, request);
}

int8_t sys_write(struct EXT2DriverRequest *request) {
    return fs_syscall(SYSCALL_WRITE, request);
}

int8_t sys_delete(struct EXT2DriverRequest *request) {
    return fs_syscall(SYSCALL_DELETE, request);
}

char sys_getchar(void) {
    char c = 0;
    syscall(SYSCALL_GETCHAR, (uint32_t) &c, 0, 0);
    return c;
}

void sys_putchar(char c, uint8_t color) {
    syscall(SYSCALL_PUTCHAR, (uint32_t) c, color, 0);
}

void sys_puts(const char *str, uint32_t len, uint8_t color) {
    syscall(SYSCALL_PUTS, (uint32_t) str, len, color);
}

void sys_keyboard_on(void) {
    syscall(SYSCALL_KEYBOARD_ON, 0, 0, 0);
}

void sys_clear_screen(void) {
    syscall(SYSCALL_CLEAR_SCREEN, 0, 0, 0);
}

int32_t sys_exec(struct EXT2DriverRequest *request) {
    int32_t retcode = -1;
    syscall(SYSCALL_EXEC, (uint32_t) request, (uint32_t) &retcode, 0);
    return retcode;
}

uint32_t sys_ps(struct ProcessInfo *info, uint32_t capacity) {
    uint32_t count = 0;
    syscall(SYSCALL_PS, (uint32_t) info, capacity, (uint32_t) &count);
    return count;
}

int8_t sys_kill(uint32_t pid) {
    int8_t retcode = -1;
    syscall(SYSCALL_KILL, pid, (uint32_t) &retcode, 0);
    return retcode;
}

void sys_exit(void) {
    syscall(SYSCALL_EXIT, 0, 0, 0);
    // The kernel switches away and never resumes this process.
    while (true)
        ;
}

void sys_yield(void) {
    syscall(SYSCALL_YIELD, 0, 0, 0);
}

uint32_t sys_get_pid(void) {
    uint32_t pid = 0;
    syscall(SYSCALL_GET_PID, (uint32_t) &pid, 0, 0);
    return pid;
}

void sys_get_time(struct RTCTime *time) {
    syscall(SYSCALL_GET_TIME, (uint32_t) time, 0, 0);
}

void sys_putchar_at(char c, uint8_t row, uint8_t col, uint8_t color) {
    syscall(SYSCALL_PUTCHAR_AT, (uint32_t) c, color, ((uint32_t) row << 8) | col);
}
