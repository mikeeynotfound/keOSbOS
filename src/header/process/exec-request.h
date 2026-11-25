#ifndef _EXEC_REQUEST_H
#define _EXEC_REQUEST_H

#include <stdint.h>
#include <stdbool.h>
#include "header/filesystem/ext2.h"

/**
 * Shared between kernel and user programs (SYSCALL_EXEC_ARGS).
 * Used by the shell to run a separated shell command (bonus) with arguments,
 * the shell's current directory, and optional pipe input / output capture.
 */

#define EXEC_ARGC_MAX       16
#define EXEC_ARGS_SIZE_MAX  512    // total bytes of all argument strings, including '\0'
#define PIPE_BUFFER_SIZE    (32 * 1024)

// Return code of process_create_user_process() / SYSCALL_EXEC / SYSCALL_EXEC_ARGS
#define PROCESS_CREATE_SUCCESS                   0
#define PROCESS_CREATE_FAIL_MAX_PROCESS_EXCEEDED 1
#define PROCESS_CREATE_FAIL_INVALID_ENTRYPOINT   2
#define PROCESS_CREATE_FAIL_NOT_ENOUGH_MEMORY    3
#define PROCESS_CREATE_FAIL_FS_READ_FAILURE      4
#define PROCESS_CREATE_FAIL_INVALID_EXECUTABLE   5   // ELF header or segments are not valid
#define PROCESS_CREATE_FAIL_INVALID_ARGUMENT     6   // too many / too long arguments

/**
 * @param file           Program file to run. file.buf is the entrypoint for flat binaries,
 *                       ELF executables use the entrypoint from their header
 * @param argc           Number of arguments, argv[0] is the program name
 * @param argv           Argument strings (in the caller's memory)
 * @param cwd_inode      Working directory of the new process (read with SYSCALL_GET_CWD)
 * @param stdin_buf      Pipe input for the new process, NULL if there is none
 * @param stdin_len      Length of stdin_buf, at most PIPE_BUFFER_SIZE
 * @param capture_stdout true: SYSCALL_WRITE_STDOUT of the new process goes to a kernel
 *                       buffer (read later with SYSCALL_READ_STDOUT) instead of the screen
 */
struct ExecRequest {
    struct EXT2DriverRequest file;
    uint32_t    argc;
    char      **argv;
    uint32_t    cwd_inode;
    const char *stdin_buf;
    uint32_t    stdin_len;
    bool        capture_stdout;
} __attribute__((packed));

#endif
