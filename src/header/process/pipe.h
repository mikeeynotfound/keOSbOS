#ifndef _PIPE_H
#define _PIPE_H

#include <stdint.h>
#include <stdbool.h>
#include "header/process/exec-request.h"

/**
 * Kernel side pipe buffers for separated shell commands (bonus).
 * The shell runs pipeline stages one at a time and waits for each, so a single
 * input buffer and a single output buffer are enough. Each buffer belongs to
 * one PID, PIDs are never reused so a stale owner can never match a new process.
 */

/**
 * Give process pid a copy of data as its pipe input.
 * Must be called while the memory holding data is mapped (caller's address space).
 */
void pipe_set_stdin(uint32_t pid, const char *data, uint32_t len);

// Remove any pipe input, every process will see "no pipe input"
void pipe_clear_stdin(void);

/**
 * Copy the pipe input of pid into buf.
 * @return Number of bytes copied, -1 if pid has no pipe input
 */
int32_t pipe_read_stdin(uint32_t pid, char *buf, uint32_t capacity);

// Start capturing the standard output of pid, clearing the previous capture
void pipe_capture_stdout(uint32_t pid);

// Stop capturing, standard output of every process goes to the screen
void pipe_stop_capture(void);

/**
 * Standard output of pid: appended to the capture buffer if pid is being captured,
 * otherwise printed on the screen.
 */
void pipe_write_stdout(uint32_t pid, const char *s, uint32_t len, uint8_t color);

/**
 * Copy the captured output into buf.
 * @return Total captured length, can be larger than capacity (output was cut)
 *         or larger than PIPE_BUFFER_SIZE (the producer wrote more than the buffer holds)
 */
uint32_t pipe_read_stdout(char *buf, uint32_t capacity);

#endif
