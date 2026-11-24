#ifndef _USER_SYSCALL_H
#define _USER_SYSCALL_H

#include <stdint.h>
#include <stdbool.h>
#include "header/filesystem/ext2.h"
#include "header/syscall-number.h"
#include "header/process/process-info.h"
#include "header/driver/cmos.h"

/**
 * Raise int 0x30 with eax = number and ebx / ecx / edx = arguments.
 * See header/syscall-number.h for the argument layout of every syscall.
 */
void syscall(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx);

/* -- Wrappers, return codes follow the EXT2 driver functions -- */

int8_t sys_read(struct EXT2DriverRequest *request);
int8_t sys_read_directory(struct EXT2DriverRequest *request);
int8_t sys_write(struct EXT2DriverRequest *request);
int8_t sys_delete(struct EXT2DriverRequest *request);

// Get the last pressed key, 0 if there is none
char sys_getchar(void);
void sys_putchar(char c, uint8_t color);
void sys_puts(const char *str, uint32_t len, uint8_t color);
void sys_keyboard_on(void);
void sys_clear_screen(void);

/* -- Process -- */

// Start executable described by request (buf = entrypoint), returns PROCESS_CREATE_* code
int32_t  sys_exec(struct EXT2DriverRequest *request);
// Fill info with up to capacity running processes, returns the count
uint32_t sys_ps(struct ProcessInfo *info, uint32_t capacity);
// Terminate process pid, returns PROCESS_KILL_* code
int8_t   sys_kill(uint32_t pid);
// Terminate the calling process, never returns
void     sys_exit(void) __attribute__((noreturn));
// Voluntarily give up the rest of this time slice
void     sys_yield(void);
// Return the PID of the calling process
uint32_t sys_get_pid(void);

// Read current date & time from CMOS RTC
void sys_get_time(struct RTCTime *time);
// Print a character at a fixed screen position, does not move the cursor
void sys_putchar_at(char c, uint8_t row, uint8_t col, uint8_t color);

#endif
