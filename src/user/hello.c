#include "header/user/syscall.h"

int main(void) {
    sys_puts("Hello World!\n", 13, 0x7);
    return 0;
}
