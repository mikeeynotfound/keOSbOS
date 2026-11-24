#ifndef _PROCESS_INFO_H
#define _PROCESS_INFO_H

#include <stdint.h>

/**
 * Process information shared between kernel and user programs (used by SYSCALL_PS).
 * Kept free of kernel-only types so user programs can include it.
 */
#define PROCESS_NAME_LENGTH_MAX 32
#define PROCESS_COUNT_MAX       16

// Process states
#define PROCESS_STATE_INACTIVE  0
#define PROCESS_STATE_READY     1
#define PROCESS_STATE_RUNNING   2

/**
 * @param pid              Process ID
 * @param state            PROCESS_STATE_* value
 * @param page_frame_count Number of user page frames owned by the process
 * @param name             Null-terminated process name
 */
struct ProcessInfo {
    uint32_t pid;
    uint32_t state;
    uint32_t page_frame_count;
    char     name[PROCESS_NAME_LENGTH_MAX];
} __attribute__((packed));

// Return code of process_destroy() / SYSCALL_KILL
#define PROCESS_KILL_SUCCESS       0
#define PROCESS_KILL_NOT_FOUND     1
#define PROCESS_KILL_NOT_PERMITTED 2

#endif
