#ifndef _USER_SYSCALL_H
#define _USER_SYSCALL_H

#include <stdint.h>
#include <stdbool.h>
#include "header/filesystem/ext2.h"
#include "header/syscall-number.h"

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

#endif
