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
 * SYSCALL_EXEC            ebx: struct EXT2DriverRequest*, ecx: int32_t* retcode
 * SYSCALL_PS              ebx: struct ProcessInfo*, ecx: capacity, edx: uint32_t* count
 * SYSCALL_KILL            ebx: pid, ecx: int8_t* retcode
 * SYSCALL_EXIT            no argument, terminate current process and reschedule
 * SYSCALL_GET_TIME        ebx: struct RTCTime* output (see header/driver/cmos.h)
 * SYSCALL_PUTCHAR_AT      ebx: char, ecx: color, edx: (row << 8) | col
 * SYSCALL_YIELD            no argument, voluntarily give up CPU
 * SYSCALL_GET_PID          ebx: uint32_t* output
 *
 * Bonus: separate shell commands (ELF), see header/process/exec-request.h
 * SYSCALL_EXEC_ARGS        ebx: struct ExecRequest*, ecx: int32_t* retcode, edx: uint32_t* pid
 * SYSCALL_PROCESS_ALIVE    ebx: pid, ecx: bool* output (true while the process exists)
 * SYSCALL_READ_STDIN       ebx: char* buf, ecx: capacity, edx: int32_t* length (-1: no pipe input)
 * SYSCALL_WRITE_STDOUT     ebx: char*, ecx: length, edx: color (screen, or pipe when captured)
 * SYSCALL_READ_STDOUT      ebx: char* buf, ecx: capacity, edx: uint32_t* total captured length
 * SYSCALL_GET_CWD          ebx: uint32_t* output, working directory inode of the caller
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
#define SYSCALL_EXEC           9
#define SYSCALL_PS             10
#define SYSCALL_KILL           11
#define SYSCALL_EXIT           12
#define SYSCALL_GET_TIME       13
#define SYSCALL_PUTCHAR_AT     14
#define SYSCALL_YIELD          15
#define SYSCALL_GET_PID        16
#define SYSCALL_EXEC_ARGS      17
#define SYSCALL_PROCESS_ALIVE  18
#define SYSCALL_READ_STDIN     19
#define SYSCALL_WRITE_STDOUT   20
#define SYSCALL_READ_STDOUT    21
#define SYSCALL_GET_CWD        22

#endif
