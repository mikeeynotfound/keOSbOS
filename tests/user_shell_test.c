// Host test for src/user/user-shell.c: the sys_* wrappers call the EXT2 driver on an in-memory
// disk and keyboard input comes from a script.
// gcc -g -Wall -Wextra -fsanitize=address,undefined -Isrc -Dmain=shell_main -c src/user/user-shell.c -o /tmp/shell.o
// gcc -g -Wall -Wextra -fsanitize=address,undefined -Isrc tests/user_shell_test.c /tmp/shell.o -o /tmp/user_shell_test
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <assert.h>
#define read   ext2_read
#define write  ext2_write
#define delete ext2_delete
#include "header/filesystem/ext2.h"
#include "filesystem/ext2.c"
#undef read
#undef write
#undef delete
#include "header/user/syscall.h"

int shell_main(void);

static uint8_t disk[DISK_SPACE];
void read_blocks(void *p, uint32_t lba, uint8_t n) {
    assert((lba + n) * 512u <= sizeof disk);
    memcpy(p, disk + lba * 512, n * 512);
}
void write_blocks(const void *p, uint32_t lba, uint8_t n) {
    assert((lba + n) * 512u <= sizeof disk);
    memcpy(disk + lba * 512, p, n * 512);
}

/* -- Fake syscalls -- */

static const char *script;
static jmp_buf     script_done;
static char        out[1 << 16];
static size_t      out_len;

int8_t sys_read(struct EXT2DriverRequest *r)           { return ext2_read(*r); }
int8_t sys_read_directory(struct EXT2DriverRequest *r) { return read_directory(r); }
int8_t sys_write(struct EXT2DriverRequest *r)          { return ext2_write(r); }
int8_t sys_delete(struct EXT2DriverRequest *r)         { return ext2_delete(*r); }
void   sys_keyboard_on(void)                           {}
void   sys_clear_screen(void)                          { out_len = 0; }

void sys_putchar(char c, uint8_t color) {
    (void) color;
    if (c == '\b') {
        if (out_len > 0)
            out_len--;
        return;
    }
    assert(out_len < sizeof out - 1);
    out[out_len++] = c;
}

void sys_puts(const char *s, uint32_t len, uint8_t color) {
    for (uint32_t i = 0; i < len; i++)
        sys_putchar(s[i], color);
}

char sys_getchar(void) {
    if (*script == '\0')
        longjmp(script_done, 1);
    return *script++;
}

static const char *run(const char *input) {
    script  = input;
    out_len = 0;
    if (setjmp(script_done) == 0)
        shell_main();
    out[out_len] = '\0';
    return out;
}

/* -- Helpers -- */

static int checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL line %d: %s\n--- output ---\n%s\n", __LINE__, #c, out); exit(1); } } while (0)
#define HAS(s)   CHECK(strstr(out, s) != NULL)
#define LACKS(s) CHECK(strstr(out, s) == NULL)

static void put_file(uint32_t parent, const char *name, const char *data, uint32_t size) {
    char *copy = malloc(size);
    memcpy(copy, data, size);
    struct EXT2DriverRequest r = { .buf = copy, .name = (char*) name, .name_len = strlen(name),
                                   .parent_inode = parent, .buffer_size = size, .is_directory = false };
    assert(ext2_write(&r) == 0);
    free(copy);
}

// Output of the last command in a run: text after the last prompt line that ran it
static const char *after(const char *marker) {
    const char *p = strstr(out, marker);
    CHECK(p != NULL);
    return p + strlen(marker);
}

int main(void) {
    memset(disk, 0, sizeof disk);
    initialize_filesystem_ext2();
    put_file(1, "hello.txt", "hello world\n", 12);

    // Prompt, help, unknown command, empty line
    run("help\n\nfoo\n");
    HAS("keOSbOS:/$ ");
    HAS("mkdir <dir>");
    HAS("foo: command not found");

    // ls / cat on root
    run("ls\ncat hello.txt\n");
    HAS("hello.txt\n");
    LACKS("./");
    HAS("hello world\n");

    // Backspace editing and tab
    run("cat helXY\b\blo.txt\n");
    HAS("hello world\n");

    // mkdir, nested mkdir, cd, prompt path, ..
    run("mkdir docs\nmkdir docs/a\nmkdir docs/a\ncd docs/a\ncd ..\ncd ../docs/./a/\ncd /\ncd nope\n");
    HAS("mkdir: docs/a: already exists");
    HAS("keOSbOS:/docs/a$ ");
    HAS("keOSbOS:/docs$ ");
    HAS("cd: nope: no such directory");
    run("ls docs\nls\n");
    HAS("a/\n");
    HAS("docs/\n");

    // cd .. at root stays at root, cd with no argument goes to root
    run("cd ..\ncd docs\ncd\nls\n");
    CHECK(strstr(out, "keOSbOS:/$ ") != NULL);
    HAS("keOSbOS:/docs$ cd\nkeOSbOS:/$ ");

    // cat errors
    run("cat nope\ncat docs\ncat\n");
    HAS("cat: nope: no such file");
    HAS("cat: docs: is a directory");
    HAS("cat: missing operand");

    // cp to new name, into directory, nested path, existing name
    run("cp hello.txt copy.txt\ncat copy.txt\ncp hello.txt docs\ncat docs/hello.txt\n"
        "cp hello.txt docs/a/h2\ncat /docs/a/h2\ncp hello.txt copy.txt\ncp nope x\n");
    CHECK(strstr(after("cat copy.txt\n"), "hello world\n") == after("cat copy.txt\n"));
    CHECK(strstr(after("cat docs/hello.txt\n"), "hello world\n") == after("cat docs/hello.txt\n"));
    CHECK(strstr(after("cat /docs/a/h2\n"), "hello world\n") == after("cat /docs/a/h2\n"));
    HAS("cp: copy.txt: already exists");
    HAS("cp: nope: no such file");

    // Exact size is preserved, including a binary file with zero bytes and no trailing newline
    {
        static char big[3000];
        for (int i = 0; i < 3000; i++)
            big[i] = (i % 7 == 0) ? 0 : (char) ('a' + i % 26);
        put_file(1, "big.bin", big, 3000);
        run("cp big.bin big2.bin\n");
        static char back[4000];
        struct EXT2DriverRequest r = { .buf = back, .name = "big2.bin", .name_len = 8, .parent_inode = 1,
                                       .buffer_size = 3000, .is_directory = false };
        CHECK(ext2_read(r) == 0);
        CHECK(memcmp(back, big, 3000) == 0);
        r.buffer_size = 2999;
        CHECK(ext2_read(r) == 2); // not a byte bigger than the source
    }
    run("cat big.bin\n"); // no newline at the end of the file, shell adds one before the prompt
    HAS("\nkeOSbOS:/$ ");

    // find
    run("find hello.txt\nfind a\nfind zzz\n");
    HAS("/hello.txt\n");
    HAS("/docs/hello.txt\n");
    HAS("/docs/a\n");
    HAS("find: zzz: not found");

    // find across a directory big enough to span several blocks (dir_buffer reload after recursion)
    {
        char cmd[64 * 40] = "mkdir many\n";
        for (int i = 0; i < 60; i++)
            sprintf(cmd + strlen(cmd), "mkdir many/dir_number_%02d\n", i);
        strcat(cmd, "cp hello.txt many/dir_number_59\nfind hello.txt\n");
        run(cmd);
        HAS("/many/dir_number_59/hello.txt\n");
        run("ls many\n");
        HAS("dir_number_00/\n");
        HAS("dir_number_59/\n");
    }

    // rm and mv: results depend on delete(), report what happens
    run("rm docs\nrm nope\nrm\n");
    HAS("rm: docs: directory not empty");
    HAS("rm: nope: no such file or directory");
    HAS("rm: missing operand");

    run("mv copy.txt moved.txt\ncat moved.txt\n");
    CHECK(strstr(after("cat moved.txt\n"), "hello world\n") == after("cat moved.txt\n"));
    run("ls\n");
    bool copy_gone = strstr(out, "copy.txt\n") == NULL;

    run("mkdir empty\nrm empty\nls\n");
    bool empty_gone = strstr(after("rm empty\n"), "empty/\n") == NULL;

    // clear
    run("ls\nclear\n");
    CHECK(strncmp(out, "keOSbOS:/$ ", 11) == 0);

    printf("ALL OK (%d checks)\n", checks);
    printf("delete() status: rm file %s, rm empty dir %s\n",
           copy_gone ? "removes it" : "DOES NOT remove it",
           empty_gone ? "removes it" : "DOES NOT remove it");
    return 0;
}
