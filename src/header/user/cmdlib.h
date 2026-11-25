#ifndef _CMDLIB_H
#define _CMDLIB_H

#include <stdint.h>
#include <stdbool.h>
#include "header/user/syscall.h"

/**
 * Helpers shared by the separated shell commands in src/user/cmd (bonus, book 4.3.6).
 * Paths are resolved relative to the working directory the shell gave the process
 * (SYSCALL_GET_CWD). Normal output goes through SYSCALL_WRITE_STDOUT so it can feed
 * a pipe, errors are always printed on the screen.
 */

#define ROOT_INODE        1
#define PATH_MAX          256
#define DIR_BUFFER_SIZE   (32 * BLOCK_SIZE)
#define FILE_BUFFER_SIZE  (128 * BLOCK_SIZE)

#define COLOR_TEXT   0x7
#define COLOR_DIR    0xB
#define COLOR_ERROR  0xC

extern uint8_t dir_buffer[DIR_BUFFER_SIZE];
extern uint8_t file_buffer[FILE_BUFFER_SIZE];
extern char    stdin_buffer[PIPE_BUFFER_SIZE];

/* -- Strings -- */
uint32_t str_len(const char *s);
bool     name_equal(const char *a, uint32_t a_len, const char *b, uint32_t b_len);

/* -- Regex (small POSIX-like subset used by grep) -- */
// Validate a pattern. Supports literals, '.', '^', '$', '*', '+', '?', '[]', '\\' and '|'
bool regex_valid(const char *pattern, uint32_t pattern_len);
// Match anywhere in text[0..len). '^' anchors to the start, '$' to the end of the text
bool regex_match(const char *text, uint32_t len, const char *pattern, uint32_t pattern_len);

/* -- Output -- */
// Standard output (screen, or the pipe when this command is not the last stage)
void out_puts(const char *s, uint32_t len, uint8_t color);
void out_putchar(char c, uint8_t color);
void out_print(const char *s, uint8_t color);
// Error output, always on the screen: "<cmd>: <arg>: <msg>"
void print_error(const char *cmd, const char *arg, const char *msg);

/* -- Input -- */
// Read the pipe input into stdin_buffer. Returns its length, -1 if there is no pipe input
int32_t read_stdin(void);

/* -- File system -- */
// Working directory inode given by the shell
uint32_t cwd_inode(void);
// Read the table of directory inode into dir_buffer
bool load_dir(uint32_t inode);
char *entry_name(struct EXT2DirectoryEntry *entry);
// Next used entry of dir_buffer starting at *offset, NULL at the end of the table
struct EXT2DirectoryEntry *next_entry(uint32_t *offset);
bool is_dot_entry(struct EXT2DirectoryEntry *entry);
// Find name inside directory dir. Returns the inode (0 if missing) and sets *type
uint32_t lookup(uint32_t dir, const char *name, uint32_t name_len, uint8_t *type);
// Resolve path[0..len) to a directory inode, relative to cwd unless it starts with '/'. 0 if not found
uint32_t resolve_dir(const char *path, uint32_t len);
// Split path into its parent directory inode and last component. False if the parent does not exist
bool split_path(const char *path, uint32_t *parent, const char **name, uint32_t *name_len);
// Read the file at path into file_buffer and set *size. Prints an error and returns false on failure
bool load_file(const char *cmd, const char *path, uint32_t *size);

/* -- File operations shared by cp, mv and rm -- */
// Copy file src to dst (a new name, or an existing directory). Prints errors, returns true on success
bool copy_file(const char *cmd, const char *src, const char *dst);
// Delete a file, or an empty directory. Returns the delete() return code (0: success)
int8_t remove_path(const char *path, bool is_directory);

#endif
