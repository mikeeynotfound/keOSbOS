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
