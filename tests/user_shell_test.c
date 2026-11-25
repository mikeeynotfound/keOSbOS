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

int32_t sys_exec(struct EXT2DriverRequest *r) {
    (void) r;
    return 4; // no process creation in the host harness
}

/*
 * Separated shell commands (bonus): every program in /bin is linked into this test with
 * main renamed to cmd_<name>_main. SYSCALL_EXEC_ARGS is emulated by running that main
 * right away with the cwd, pipe input and output capture the shell asked for, so the
 * "process" has already exited when the shell starts waiting for it.
 */
#define CMD(name) int cmd_##name##_main(int argc, char **argv);
CMD(ls) CMD(mkdir) CMD(cat) CMD(cp) CMD(rm) CMD(mv) CMD(find) CMD(grep)
static const struct { const char *name; int (*main)(int, char **); } programs[] = {
    {"ls", cmd_ls_main}, {"mkdir", cmd_mkdir_main}, {"cat", cmd_cat_main}, {"cp", cmd_cp_main},
    {"rm", cmd_rm_main}, {"mv", cmd_mv_main}, {"find", cmd_find_main}, {"grep", cmd_grep_main},
};

static uint32_t    fake_cwd = 1;
static const char *fake_stdin;
static int32_t     fake_stdin_len = -1;
static bool        fake_capture;
static char        captured[PIPE_BUFFER_SIZE];
static uint32_t    captured_len;
static uint32_t    next_pid = 1;
static char        last_argv[EXEC_ARGC_MAX][64];   // what the last program received
static uint32_t    last_argc;

int32_t sys_exec_args(const struct ExecRequest *req, uint32_t *pid) {
    // The file must exist, like get_file_size() in the kernel
    char probe[1];
    struct EXT2DriverRequest r = req->file;
    r.buf         = probe;
    r.buffer_size = sizeof probe;
    int8_t found = ext2_read(r);
    if (found != 0 && found != 2)
        return PROCESS_CREATE_FAIL_FS_READ_FAILURE;

    int (*program)(int, char **) = NULL;
    for (size_t i = 0; i < sizeof programs / sizeof programs[0]; i++)
        if (strlen(programs[i].name) == r.name_len && memcmp(programs[i].name, r.name, r.name_len) == 0)
            program = programs[i].main;
    if (program == NULL)
        return PROCESS_CREATE_FAIL_INVALID_EXECUTABLE;
    if (req->argc > EXEC_ARGC_MAX)
        return PROCESS_CREATE_FAIL_INVALID_ARGUMENT;

    last_argc = req->argc;
    for (uint32_t i = 0; i < req->argc; i++)
        snprintf(last_argv[i], sizeof last_argv[i], "%s", req->argv[i]);

    fake_cwd       = req->cwd_inode;
    fake_stdin     = req->stdin_buf;
    fake_stdin_len = req->stdin_buf ? (int32_t) req->stdin_len : -1;
    fake_capture   = req->capture_stdout;
    captured_len   = 0;
    *pid = next_pid++;

    program((int) req->argc, req->argv);

    fake_stdin_len = -1;
    fake_capture   = false;
    return PROCESS_CREATE_SUCCESS;
}

bool sys_process_alive(uint32_t pid) {
    (void) pid;
    return false; // the emulated program already ran to completion
}

void sys_wait(uint32_t pid) {
    (void) pid;
}

int32_t sys_read_stdin(char *buf, uint32_t capacity) {
    if (fake_stdin_len < 0)
        return -1;
    uint32_t n = (uint32_t) fake_stdin_len < capacity ? (uint32_t) fake_stdin_len : capacity;
    memcpy(buf, fake_stdin, n);
    return (int32_t) n;
}

void sys_write_stdout(const char *s, uint32_t len, uint8_t color) {
    if (!fake_capture) {
        sys_puts(s, len, color);
        return;
    }
    for (uint32_t i = 0; i < len; i++) {
        if (captured_len < PIPE_BUFFER_SIZE)
            captured[captured_len] = s[i];
        captured_len++;
    }
}

uint32_t sys_read_stdout(char *buf, uint32_t capacity) {
    uint32_t n = captured_len < capacity ? captured_len : capacity;
    if (n > PIPE_BUFFER_SIZE)
        n = PIPE_BUFFER_SIZE;
    memcpy(buf, captured, n);
    return captured_len;
}

uint32_t sys_get_cwd(void) {
    return fake_cwd;
}

uint32_t sys_ps(struct ProcessInfo *info, uint32_t capacity) {
    if (capacity == 0)
        return 0;
    info[0].pid              = 0;
    info[0].state            = PROCESS_STATE_RUNNING;
    info[0].page_frame_count = 128;
    memset(info[0].name, 0, sizeof(info[0].name));
    memcpy(info[0].name, "shell", 5);
    return 1;
}

int8_t sys_kill(uint32_t pid) {
    (void) pid;
    return PROCESS_KILL_NOT_FOUND;
}

uint32_t sys_get_pid(void) {
    return 0;
}

void sys_yield(void) {}
void sys_exit(void) { for (;;) {} }

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
    {
        // /bin holds the separated commands (contents do not matter, the harness runs them by name)
        struct EXT2DriverRequest d = { .buf = NULL, .name = "bin", .name_len = 3, .parent_inode = 1,
                                       .buffer_size = 0, .is_directory = true };
        assert(ext2_write(&d) == 0);
        struct EXT2Inode root;
        assert(load_parent_directory(1, &root));
        uint32_t bin = find_entry(&root, "bin", 3, EXT2_FT_DIR);
        assert(bin != 0);
        for (size_t i = 0; i < sizeof programs / sizeof programs[0]; i++)
            put_file(bin, programs[i].name, "\x7f" "ELF", 4);
    }

    // Prompt, help, unknown command, empty line
    run("help\n\nfoo\n");
    HAS("keOSbOS:/$ ");
    HAS("mkdir <dir>");
    HAS("exec <file>");
    HAS("foo: command not found");

    // ps shows the init shell
    run("ps\n");
    HAS("PID");
    HAS("shell (this shell)");

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

    // Separate shell commands (bonus): argv, cwd, pipes between processes, background
    run("grep \"lo wo\" hello.txt\n");
    HAS("hello world\n");
    CHECK(last_argc == 3);
    CHECK(strcmp(last_argv[0], "grep") == 0);
    CHECK(strcmp(last_argv[1], "lo wo") == 0);
    CHECK(strcmp(last_argv[2], "hello.txt") == 0);

    // Regex grep (bonus): literal, '.', '^', '$', quantifiers, class, escape and '|'
    {
        const char *rx = "alpha\nbeta\nalpha beta\naab\nab\nb\na.b\ncat dog\n";
        put_file(1, "regex.txt", rx, strlen(rx));
    }
    {
        const char *m, *e;

        m = "grep alpha regex.txt\n";  e = "alpha\nalpha beta\n";
        run(m); CHECK(strncmp(after(m), e, strlen(e)) == 0);

        m = "grep \"a.b\" regex.txt\n"; e = "alpha beta\naab\na.b\n";
        run(m); CHECK(strncmp(after(m), e, strlen(e)) == 0);

        m = "grep \"^beta$\" regex.txt\n"; e = "beta\n";
        run(m); CHECK(strncmp(after(m), e, strlen(e)) == 0);

        m = "grep \"beta$\" regex.txt\n"; e = "beta\nalpha beta\n";
        run(m); CHECK(strncmp(after(m), e, strlen(e)) == 0);

        m = "grep \"^a*b$\" regex.txt\n"; e = "aab\nab\nb\n";
        run(m); CHECK(strncmp(after(m), e, strlen(e)) == 0);

        m = "grep \"^a+b$\" regex.txt\n"; e = "aab\nab\n";
        run(m); CHECK(strncmp(after(m), e, strlen(e)) == 0);

        m = "grep \"^ab?$\" regex.txt\n"; e = "ab\n";
        run(m); CHECK(strncmp(after(m), e, strlen(e)) == 0);

        m = "grep \"^a[ab]b$\" regex.txt\n"; e = "aab\n";
        run(m); CHECK(strncmp(after(m), e, strlen(e)) == 0);

        m = "grep \"^[^a]\" regex.txt\n"; e = "beta\nb\ncat dog\n";
        run(m); CHECK(strncmp(after(m), e, strlen(e)) == 0);

        m = "grep \"^beta$|^b$\" regex.txt\n"; e = "beta\nb\n";
        run(m); CHECK(strncmp(after(m), e, strlen(e)) == 0);

        m = "grep \"a\\.b\" regex.txt\n"; e = "a.b\n";
        run(m); CHECK(strncmp(after(m), e, strlen(e)) == 0);

        run("grep \"[\" regex.txt\n");
        HAS("grep: [: invalid regex");

        run("cat regex.txt | grep \"^cat\"\n");
        HAS("cat dog\n");
    }

    run("cd docs\nls\ncat hello.txt\n");               // relative paths use the shell's cwd
    HAS("a/\n");
    CHECK(strstr(after("cat hello.txt\n"), "hello world\n") == after("cat hello.txt\n"));

    run("cd /\nls | grep hello\n");                  // program -> program
    CHECK(strstr(after("ls | grep hello\n"), "hello.txt\n") == after("ls | grep hello\n"));
    LACKS("docs/\n");
    run("cat hello.txt | grep world | cat\n");        // three processes
    CHECK(strstr(after("| cat\n"), "hello world\n") == after("| cat\n"));
    run("ps | grep shell\n");                         // built-in -> program
    HAS("shell (this shell)");
    run("ls | grep zzz\ncat | grep x\n");
    CHECK(strstr(after("ls | grep zzz\n"), "keOSbOS:/$ ") == after("ls | grep zzz\n"));
    HAS("cat: missing operand");                     // first stage has no pipe input

    run("ls &\nexec /bin/ls\nexec /bin/nope\n");
    HAS("started ls with pid ");
    HAS("exec: /bin/nope: no such file");

    // PATH (bonus): default "/bin:/", lookup order, export, saved in /.path.
    // "prog" is not a program the harness knows, so the error shows the resolved path.
    put_file(1, "prog", "\x90", 1);
    run("export\nprog\n");
    HAS("PATH=/bin:/\n");
    HAS("prog: /prog: not a valid executable");
    run("mkdir bin\nexport PATH=/bin\nprog\n");
    HAS("prog: command not found");
    run("cp /prog /bin\nprog\nexport PATH=nope::/bin/\nprog\nexport FOO=1\n");
    HAS("prog: /bin/prog: not a valid executable");
    LACKS("prog: command not found");
    HAS("export: FOO=1: only PATH=");
    {
        char saved[32] = {0};
        struct EXT2DriverRequest r = { .buf = saved, .name = ".path", .name_len = 5,
                                       .parent_inode = 1, .buffer_size = sizeof saved };
        CHECK(ext2_read(r) == 0);
        CHECK(strcmp(saved, "nope::/bin/\n") == 0);
    }
    run("export\n");                   // a new shell run reloads PATH from /.path
    HAS("PATH=nope::/bin/\n");

    // Without /bin in PATH the separated commands are not found
    run("export PATH=/\nls\nexport PATH=/bin:/\nls\n");
    HAS("ls: command not found");
    HAS("hello.txt\n");

    // export can add folders with $PATH and remove them by rewriting the list
    run("export PATH=/bin\nexport PATH=$PATH:/apps\nexport\nexport PATH=/x:$PATH\nexport\nexport PATH=/bin\nexport\n");
    HAS("PATH=/bin:/apps\n");
    HAS("PATH=/x:/bin:/apps\n");
    CHECK(strstr(after("PATH=/x:/bin:/apps\n"), "PATH=/bin\n") != NULL);
    run("export PATH=$PATH:/\nhello.txt\n");     // /bin:/ again, the root file is found through PATH
    HAS("hello.txt: /hello.txt: not a valid executable");

    // Recursive folder support: cp -r / rm -r (book bonus 3.4.2.2)
    run("export PATH=/bin:/\nmkdir tree\nmkdir tree/sub\nmkdir tree/sub/deep\n"
        "cp hello.txt tree/a.txt\ncp big.bin tree/sub/b.bin\n");
    {
        struct EXT2Inode rootnode;
        CHECK(load_parent_directory(ROOT_INODE, &rootnode));
        uint32_t tree = find_entry(&rootnode, "tree", 4, EXT2_FT_DIR);
        CHECK(tree != 0);
        put_file(tree, "empty.txt", "", 0); // empty regular file, not a directory
    }
    run("cp -r tree tree_copy\nls tree_copy\nls tree_copy/sub\nls tree_copy/sub/deep\n"
        "cat tree_copy/a.txt\n");
    HAS("sub/\n");
    HAS("a.txt\n");
    HAS("empty.txt\n");
    LACKS("empty.txt/\n"); // empty regular file, not a directory
    HAS("deep/\n");
    HAS("b.bin\n");
    CHECK(strstr(after("cat tree_copy/a.txt\n"), "hello world\n") == after("cat tree_copy/a.txt\n"));

    run("cp -r tree tree/sub/bad\n"); // cannot copy a directory into itself
    HAS("cp: tree/sub/bad: cannot copy a directory into itself");

    run("rm -r /\nrm -r .\nrm -r tree_copy\nfind tree_copy\n");
    HAS("rm: /: cannot remove root or current directory");
    HAS("rm: .: cannot remove root or current directory");
    HAS("find: tree_copy: not found");

    printf("ALL OK (%d checks)\n", checks);
    printf("delete() status: rm file %s, rm empty dir %s\n",
           copy_gone ? "removes it" : "DOES NOT remove it",
           empty_gone ? "removes it" : "DOES NOT remove it");
    return 0;
}
