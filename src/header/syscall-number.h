#ifndef _SYSCALL_NUMBER_H
#define _SYSCALL_NUMBER_H

/**
 * System call numbers, shared by the kernel handler and user programs.
 * Calling convention: int 0x30 with eax = number, ebx / ecx / edx = arguments.
 * Return values are written through pointers given in the arguments.
 *
 * SYSCALL_READ            ebx: struct EXT2DriverRequest*, ecx: int8_t* retcode
 * SYSCALL_READ_DIRECTORY  ebx: struct EXT2DriverRequest*, ecx: int8_t* retcode
 * SYSCALL_WRITE           ebx: struct EXT2DriverRequest*, ecx: int8_t* retcode
 * SYSCALL_DELETE          ebx: struct EXT2DriverRequest*, ecx: int8_t* retcode
 * SYSCALL_GETCHAR         ebx: char* (0 if no key was pressed)
 * SYSCALL_PUTCHAR         ebx: char, ecx: color
 * SYSCALL_PUTS            ebx: char*, ecx: length, edx: color
 * SYSCALL_KEYBOARD_ON     no argument, start listening to the keyboard
 * SYSCALL_CLEAR_SCREEN    no argument, clear screen and reset cursor
 */
#define SYSCALL_READ           0
#define SYSCALL_READ_DIRECTORY 1
#define SYSCALL_WRITE          2
#define SYSCALL_DELETE         3
#define SYSCALL_GETCHAR        4
#define SYSCALL_PUTCHAR        5
#define SYSCALL_PUTS           6
#define SYSCALL_KEYBOARD_ON    7
#define SYSCALL_CLEAR_SCREEN   8

#endif
