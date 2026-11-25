#include <stdint.h>
#include <stdbool.h>
#include "header/user/syscall.h"
#include "header/stdlib/string.h"

#define ROOT_INODE      1
#define LINE_MAX        256
#define PATH_MAX        256
#define ARG_MAX         EXEC_ARGC_MAX
#define DIR_BUFFER_SIZE (32 * BLOCK_SIZE)
#define FILE_BUFFER_SIZE (2 * BLOCK_SIZE)   // only used to read /.path
#define PIPE_MAX        8
#define KEY_ESCAPE      0x1B

#define COLOR_TEXT   0x7
#define COLOR_PROMPT 0xA
#define COLOR_PATH   0x9
#define COLOR_DIR    0xB
#define COLOR_ERROR  0xC

static char     line[LINE_MAX];
static uint8_t  dir_buffer[DIR_BUFFER_SIZE];
static uint8_t  file_buffer[FILE_BUFFER_SIZE];

// Pipeline: stage N writes into pipe_out when capture_output is set, then it becomes pipe_in of stage N+1
static char     pipe_in[PIPE_BUFFER_SIZE];
static char     pipe_out[PIPE_BUFFER_SIZE];
static uint32_t pipe_in_len;
static uint32_t pipe_out_len;
static bool     capture_output;
static bool     output_truncated;

static uint32_t cwd_inode = ROOT_INODE;
static char     cwd_path[PATH_MAX] = "/";
static uint32_t cwd_path_len = 1;

/* -- String & output helpers -- */

static uint32_t str_len(const char *s) {
    uint32_t len = 0;
    while (s[len])
        len++;
    return len;
}

static bool name_equal(const char *a, uint32_t a_len, const char *b, uint32_t b_len) {
    return a_len == b_len && memcmp(a, b, a_len) == 0;
}

static void print(const char *s, uint8_t color) {
    sys_puts(s, str_len(s), color);
}

static void print_error(const char *cmd, const char *arg, const char *msg) {
    print(cmd, COLOR_ERROR);
    print(": ", COLOR_ERROR);
    if (arg) {
        print(arg, COLOR_ERROR);
        print(": ", COLOR_ERROR);
    }
    print(msg, COLOR_ERROR);
    sys_putchar('\n', COLOR_ERROR);
}

/* -- Command output: goes to the screen, or into pipe_out when the command feeds a pipe -- */

static void out_puts(const char *s, uint32_t len, uint8_t color) {
    if (!capture_output) {
        sys_puts(s, len, color);
        return;
    }
    for (uint32_t i = 0; i < len; i++) {
        if (pipe_out_len >= PIPE_BUFFER_SIZE) {
            output_truncated = true;
            return;
        }
        pipe_out[pipe_out_len++] = s[i];
    }
}

static void out_putchar(char c, uint8_t color) {
    out_puts(&c, 1, color);
}

static void out_print(const char *s, uint8_t color) {
    out_puts(s, str_len(s), color);
}

/* -- Directory helpers -- */

// Read the table of directory `inode` into dir_buffer, the "." entry lets us address it by inode
static bool load_dir(uint32_t inode) {
    struct EXT2DriverRequest req = {
        .buf          = dir_buffer,
        .name         = ".",
        .name_len     = 1,
        .parent_inode = inode,
        .buffer_size  = DIR_BUFFER_SIZE,
        .is_directory = true,
    };
    memset(dir_buffer, 0, DIR_BUFFER_SIZE);
    return sys_read_directory(&req) == 0;
}

static char *entry_name(struct EXT2DirectoryEntry *entry) {
    return (char*) entry + sizeof(struct EXT2DirectoryEntry);
}

// Next used entry of dir_buffer starting at *offset, NULL at the end of the table
static struct EXT2DirectoryEntry *next_entry(uint32_t *offset) {
    while (*offset + sizeof(struct EXT2DirectoryEntry) <= DIR_BUFFER_SIZE) {
        struct EXT2DirectoryEntry *entry = (struct EXT2DirectoryEntry*) (dir_buffer + *offset);
        // Unused blocks are zeroed by load_dir, a real entry is always at least 12 bytes
        if (entry->rec_len < 12)
            return NULL;
        *offset += entry->rec_len;
        if (entry->inode != 0)
            return entry;
    }
    return NULL;
}

// Find `name` inside directory `dir`. Returns the inode (0 if missing) and sets *type
static uint32_t lookup(uint32_t dir, const char *name, uint32_t name_len, uint8_t *type) {
    if (!load_dir(dir))
        return 0;
    uint32_t offset = 0;
    struct EXT2DirectoryEntry *entry;
    while ((entry = next_entry(&offset)) != NULL) {
        if (name_equal(entry_name(entry), entry->name_len, name, name_len)) {
            *type = entry->file_type;
            return entry->inode;
        }
    }
    return 0;
}

// Resolve path[0..len) to a directory inode, relative to cwd unless it starts with '/'. 0 if not found
static uint32_t resolve_dir(const char *path, uint32_t len) {
    uint32_t inode = (len > 0 && path[0] == '/') ? ROOT_INODE : cwd_inode;
    uint32_t i = 0;
    while (i < len) {
        while (i < len && path[i] == '/')
            i++;
        uint32_t start = i;
        while (i < len && path[i] != '/')
            i++;
        uint32_t comp_len = i - start;
        if (comp_len == 0 || name_equal(path + start, comp_len, ".", 1))
            continue;

        uint8_t type = 0;
        inode = lookup(inode, path + start, comp_len, &type);
        if (inode == 0 || type != EXT2_FT_DIR)
            return 0;
    }
    return inode;
}

/**
 * Split path into its parent directory inode and last component.
 * Returns false if the parent does not exist or the last component is empty.
 */
static bool split_path(const char *path, uint32_t *parent, const char **name, uint32_t *name_len) {
    uint32_t len = str_len(path);
    while (len > 1 && path[len - 1] == '/')
        len--;

    uint32_t slash = len;
    while (slash > 0 && path[slash - 1] != '/')
        slash--;

    *name     = path + slash;
    *name_len = len - slash;
    if (*name_len == 0 || *name_len > 255)
        return false;

    if (slash == 0)
        *parent = cwd_inode;
    else if (slash == 1)
        *parent = ROOT_INODE;
    else
        *parent = resolve_dir(path, slash - 1);
    return *parent != 0;
}

/* -- Commands -- */

static void cmd_help(void) {
    out_print("Built-in commands:\n", COLOR_TEXT);
    out_print("  cd [dir]        change directory\n", COLOR_TEXT);
    out_print("  exec <file> [args]  run program in the background\n", COLOR_TEXT);
    out_print("  ps              list running processes\n", COLOR_TEXT);
    out_print("  kill <pid>      terminate a process\n", COLOR_TEXT);
    out_print("  export [PATH=a:b]  show or set PATH (saved in /.path)\n", COLOR_TEXT);
    out_print("                  add: export PATH=$PATH:/dir\n", COLOR_TEXT);
    out_print("  clear           clear screen\n", COLOR_TEXT);
    out_print("Programs in /bin (separate ELF executables, found through PATH):\n", COLOR_TEXT);
    out_print("  ls [dir]        list directory\n", COLOR_TEXT);
    out_print("  mkdir <dir>     create directory\n", COLOR_TEXT);
    out_print("  cat [file]      print file (or pipe input)\n", COLOR_TEXT);
    out_print("  cp <src> <dst>  copy file\n", COLOR_TEXT);
    out_print("  rm <path>       remove file or empty directory\n", COLOR_TEXT);
    out_print("  mv <src> <dst>  move or rename file\n", COLOR_TEXT);
    out_print("  find <name>     search by name from /\n", COLOR_TEXT);
    out_print("  grep <pat> [file]  print lines containing pat\n", COLOR_TEXT);
    out_print("  <program> [args] [&]  run a program from PATH, & = background\n", COLOR_TEXT);
    out_print("  ESC             stop the program running in the foreground\n", COLOR_TEXT);
    out_print("  a | b           pipe output of a into b\n", COLOR_TEXT);
    out_print("                  e.g. ls | grep txt, cat f | grep \"a b\"\n", COLOR_TEXT);
}

// Apply path to cwd_path textually (inode resolution is already done by the caller)
static bool update_cwd_path(const char *path) {
    char     next[PATH_MAX];
    uint32_t next_len;
    if (path[0] == '/') {
        next[0]  = '/';
        next_len = 1;
    } else {
        memcpy(next, cwd_path, cwd_path_len);
        next_len = cwd_path_len;
    }

    uint32_t len = str_len(path), i = 0;
    while (i < len) {
        while (i < len && path[i] == '/')
            i++;
        uint32_t start = i;
        while (i < len && path[i] != '/')
            i++;
        uint32_t comp_len = i - start;

        if (comp_len == 0 || name_equal(path + start, comp_len, ".", 1))
            continue;
        if (name_equal(path + start, comp_len, "..", 2)) {
            while (next_len > 1 && next[next_len - 1] != '/')
                next_len--;
            if (next_len > 1)
                next_len--; // drop the separator too, root stays "/"
            continue;
        }

        uint32_t extra = comp_len + (next_len > 1 ? 1 : 0);
        if (next_len + extra >= PATH_MAX)
            return false;
        if (next_len > 1)
            next[next_len++] = '/';
        memcpy(next + next_len, path + start, comp_len);
        next_len += comp_len;
    }

    memcpy(cwd_path, next, next_len);
    cwd_path[next_len] = '\0';
    cwd_path_len       = next_len;
    return true;
}

static void cmd_cd(int argc, char **argv) {
    const char *path = argc >= 2 ? argv[1] : "/";
    uint32_t inode = resolve_dir(path, str_len(path));
    if (inode == 0) {
        print_error("cd", path, "no such directory");
        return;
    }
    if (!update_cwd_path(path)) {
        print_error("cd", path, "path too long");
        return;
    }
    cwd_inode = inode;
}

/* -- Process commands -- */

static void out_uint(uint32_t value, uint32_t width, uint8_t color) {
    char     buf[10];
    uint32_t len = 0;
    do {
        buf[len++] = '0' + value % 10;
        value /= 10;
    } while (value && len < sizeof(buf));
    for (uint32_t i = len; i < width; i++)
        out_putchar(' ', color);
    while (len--)
        out_putchar(buf[len], color);
}

static void out_padded(const char *s, uint32_t width, uint8_t color) {
    uint32_t len = str_len(s);
    out_puts(s, len, color);
    for (uint32_t i = len; i < width; i++)
        out_putchar(' ', color);
}

static bool parse_uint(const char *s, uint32_t *value) {
    if (*s == '\0')
        return false;
    uint32_t result = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || result > 100000000u)
            return false;
        result = result * 10 + (uint32_t) (*s - '0');
    }
    *value = result;
    return true;
}

static void print_exec_error(const char *cmd, const char *path, int32_t ret) {
    switch (ret) {
        case PROCESS_CREATE_FAIL_MAX_PROCESS_EXCEEDED: print_error(cmd, path, "too many processes"); break;
        case PROCESS_CREATE_FAIL_INVALID_ENTRYPOINT:   print_error(cmd, path, "invalid entrypoint"); break;
        case PROCESS_CREATE_FAIL_NOT_ENOUGH_MEMORY:    print_error(cmd, path, "not enough memory or program too large"); break;
        case PROCESS_CREATE_FAIL_FS_READ_FAILURE:      print_error(cmd, path, "no such file"); break;
        case PROCESS_CREATE_FAIL_INVALID_EXECUTABLE:   print_error(cmd, path, "not a valid executable"); break;
        case PROCESS_CREATE_FAIL_INVALID_ARGUMENT:     print_error(cmd, path, "too many or too long arguments"); break;
        default:                                       print_error(cmd, path, "failed"); break;
    }
}

/**
 * Wait until the foreground process pid exits, giving the CPU to other processes meanwhile.
 * ESC stops it, so a program that never ends (e.g. clock) cannot lock the shell.
 */
static void wait_foreground(uint32_t pid) {
    while (sys_process_alive(pid)) {
        if (sys_getchar() == KEY_ESCAPE) {
            sys_kill(pid);
            print("^[ stopped\n", COLOR_ERROR);
            break;
        }
        sys_yield();
    }
}

/**
 * Run the program at path as a new process with argv (argv[0] = command name).
 * Foreground: the shell gives it its cwd and pipe input, waits for it to exit (no zombie
 * or orphan is left behind), then collects its output when it feeds a pipe.
 * Background (exec, or a trailing &): the shell only prints the new PID.
 */
static void run_program(const char *cmd, const char *path, int argc, char **argv,
                        bool has_input, bool background) {
    uint32_t    parent, name_len;
    const char *name;
    if (!split_path(path, &parent, &name, &name_len)) {
        print_error(cmd, path, "no such file");
        return;
    }

    bool capture = capture_output && !background;
    struct ExecRequest req = {
        .file = {
            .buf          = (void*) 0,   // flat binaries are linked at 0, ELF uses its own entrypoint
            .name         = (char*) name,
            .name_len     = name_len,
            .parent_inode = parent,
            .buffer_size  = 0,
            .is_directory = false,
        },
        .argc           = (uint32_t) argc,
        .argv           = argv,
        .cwd_inode      = cwd_inode,
        .stdin_buf      = has_input && !background ? pipe_in : NULL,
        .stdin_len      = pipe_in_len,
        .capture_stdout = capture,
    };
    uint32_t pid = 0;
    int32_t  ret = sys_exec_args(&req, &pid);
    if (ret != PROCESS_CREATE_SUCCESS) {
        print_exec_error(cmd, path, ret);
        return;
    }

    if (background) {
        out_print("started ", COLOR_TEXT);
        out_puts(name, name_len, COLOR_TEXT);
        out_print(" with pid ", COLOR_TEXT);
        out_uint(pid, 0, COLOR_TEXT);
        out_putchar('\n', COLOR_TEXT);
        return;
    }

    wait_foreground(pid);
    if (capture) {
        uint32_t total = sys_read_stdout(pipe_out, PIPE_BUFFER_SIZE);
        pipe_out_len     = total < PIPE_BUFFER_SIZE ? total : PIPE_BUFFER_SIZE;
        output_truncated = total > PIPE_BUFFER_SIZE;
    }
}

// exec <file> [args...]: always runs the program in the background
static void cmd_exec(int argc, char **argv, bool has_input) {
    if (argc < 2) {
        print_error("exec", NULL, "missing operand");
        return;
    }
    run_program("exec", argv[1], argc - 1, argv + 1, has_input, true);
}

static void cmd_ps(void) {
    struct ProcessInfo info[PROCESS_COUNT_MAX];
    uint32_t count = sys_ps(info, PROCESS_COUNT_MAX);
    uint32_t self  = sys_get_pid();

    out_print("  PID  STATE    MEM(KiB)  NAME\n", COLOR_DIR);
    for (uint32_t i = 0; i < count; i++) {
        out_uint(info[i].pid, 5, COLOR_TEXT);
        out_print("  ", COLOR_TEXT);
        const char *state = info[i].state == PROCESS_STATE_RUNNING ? "running" : "ready";
        out_padded(state, 9, COLOR_TEXT);
        out_uint(info[i].page_frame_count * 4, 8, COLOR_TEXT);
        out_print("  ", COLOR_TEXT);
        out_print(info[i].name, COLOR_TEXT);
        if (info[i].pid == self)
            out_print(" (this shell)", COLOR_TEXT);
        out_putchar('\n', COLOR_TEXT);
    }
}

static void cmd_kill(int argc, char **argv) {
    if (argc < 2) {
        print_error("kill", NULL, "missing operand");
        return;
    }
    uint32_t pid;
    if (!parse_uint(argv[1], &pid)) {
        print_error("kill", argv[1], "invalid pid");
        return;
    }
    int8_t ret = sys_kill(pid);
    if (ret == PROCESS_KILL_NOT_FOUND)
        print_error("kill", argv[1], "no such process");
    else if (ret == PROCESS_KILL_NOT_PERMITTED)
        print_error("kill", argv[1], "cannot kill the init shell");
    else if (ret != PROCESS_KILL_SUCCESS)
        print_error("kill", argv[1], "failed");
}

/* -- PATH (bonus): search programs without exec, saved in /.path -- */

#define PATH_FILE_NAME     ".path"   // in root, so it survives a reboot
#define PATH_FILE_NAME_LEN 5
#define PATH_VAR_MAX       128

static char     path_var[PATH_VAR_MAX] = "/bin:/";   // default: commands in /bin, programs in root
static uint32_t path_var_len = 6;

// Read /.path once at shell start. Keep the default if it does not exist yet.
static void path_load(void) {
    struct EXT2DriverRequest req = {
        .buf          = file_buffer,
        .name         = PATH_FILE_NAME,
        .name_len     = PATH_FILE_NAME_LEN,
        .parent_inode = ROOT_INODE,
        .buffer_size  = FILE_BUFFER_SIZE,
        .is_directory = false,
    };
    memset(file_buffer, 0, PATH_VAR_MAX);
    if (sys_read(&req) != 0)
        return;
    uint32_t len = 0;
    while (len < PATH_VAR_MAX - 1 && file_buffer[len] != '\n' && file_buffer[len] != '\0')
        len++;
    memcpy(path_var, file_buffer, len);
    path_var[len] = '\0';
    path_var_len  = len;
}

// Write PATH to /.path. write() fails if the file exists, so delete it first.
static bool path_save(void) {
    char buf[PATH_VAR_MAX + 1];
    memcpy(buf, path_var, path_var_len);
    buf[path_var_len] = '\n';   // at least 1 byte: size 0 would create a folder
    struct EXT2DriverRequest req = {
        .buf          = buf,
        .name         = PATH_FILE_NAME,
        .name_len     = PATH_FILE_NAME_LEN,
        .parent_inode = ROOT_INODE,
        .buffer_size  = path_var_len + 1,
        .is_directory = false,
    };
    sys_delete(&req);
    return sys_write(&req) == 0;
}

static void cmd_export(int argc, char **argv) {
    if (argc < 2) {
        out_print("PATH=", COLOR_TEXT);
        out_puts(path_var, path_var_len, COLOR_TEXT);
        out_putchar('\n', COLOR_TEXT);
        return;
    }
    if (memcmp(argv[1], "PATH=", 5) != 0) {
        print_error("export", argv[1], "only PATH=<dir>[:<dir>...] is supported");
        return;
    }
    // "$PATH" is replaced by the current value, so folders can be added:
    // export PATH=$PATH:/apps   or   export PATH=/apps:$PATH
    const char *value = argv[1] + 5;
    char        next[PATH_VAR_MAX];
    uint32_t    len = 0;
    while (*value) {
        const char *part     = value;
        uint32_t    part_len = 1;
        if (memcmp(value, "$PATH", 5) == 0) {
            part     = path_var;
            part_len = path_var_len;
            value   += 5;
        } else {
            value++;
        }
        if (len + part_len >= PATH_VAR_MAX) {
            print_error("export", NULL, "PATH too long");
            return;
        }
        memcpy(next + len, part, part_len);
        len += part_len;
    }
    memcpy(path_var, next, len);
    path_var[len] = '\0';
    path_var_len  = len;
    if (!path_save())
        print_error("export", NULL, "failed to save /.path");
}

/**
 * Look for program cmd in every PATH entry (separated by ':'), first match wins.
 * On success full_path holds "<dir>/<cmd>", ready for run_program().
 */
static bool find_in_path(const char *cmd, uint32_t cmd_len, char *full_path) {
    uint32_t i = 0;
    while (i < path_var_len) {
        uint32_t start = i;
        while (i < path_var_len && path_var[i] != ':')
            i++;
        uint32_t dir_len = i - start;
        i++;                                    // skip ':'
        if (dir_len == 0)
            continue;                           // empty entry is ignored, not cwd

        uint32_t dir  = resolve_dir(path_var + start, dir_len);
        uint8_t  type = 0;
        if (dir == 0 || lookup(dir, cmd, cmd_len, &type) == 0 || type != EXT2_FT_REG_FILE)
            continue;
        if (dir_len + 1 + cmd_len >= PATH_MAX)
            continue;

        uint32_t n = dir_len;
        memcpy(full_path, path_var + start, dir_len);
        if (full_path[n - 1] != '/')
            full_path[n++] = '/';
        memcpy(full_path + n, cmd, cmd_len);
        full_path[n + cmd_len] = '\0';
        return true;
    }
    return false;
}

/* -- Main loop -- */

static void read_line(void) {
    uint32_t len = 0;
    while (true) {
        char c = sys_getchar();
        if (c == 0) {
            sys_yield(); // let other processes use the CPU while waiting for input
            continue;
        }
        if (c == '\n') {
            sys_putchar('\n', COLOR_TEXT);
            break;
        }
        if (c == '\b') {
            if (len > 0) {
                len--;
                sys_putchar('\b', COLOR_TEXT);
            }
            continue;
        }
        if (c == '\t')
            c = ' ';
        if (len < LINE_MAX - 1) {
            line[len++] = c;
            sys_putchar(c, COLOR_TEXT);
        }
    }
    line[len] = '\0';
}

// Split line at every '|' outside double quotes. Returns the stage count, -1 if there are too many
static int split_pipeline(char **stages) {
    int  count    = 0;
    bool in_quote = false;
    stages[count++] = line;
    for (char *p = line; *p; p++) {
        if (*p == '"') {
            in_quote = !in_quote;
        } else if (*p == '|' && !in_quote) {
            if (count == PIPE_MAX)
                return -1;
            *p = '\0';
            stages[count++] = p + 1;
        }
    }
    return count;
}

// Split one stage into words. A word in "double quotes" may contain spaces
static int parse_args(char *stage, char **argv) {
    int  argc = 0;
    char *p   = stage;
    while (*p && argc < ARG_MAX) {
        while (*p == ' ')
            *p++ = '\0';
        if (!*p)
            break;
        if (*p == '"') {
            argv[argc++] = ++p;
            while (*p && *p != '"')
                p++;
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ')
                p++;
        }
        if (*p)
            *p++ = '\0';
    }
    return argc;
}

static void run_command(int argc, char **argv, bool has_input) {
    const char *cmd = argv[0];
    uint32_t    len = str_len(cmd);
    if (name_equal(cmd, len, "help", 4))
        cmd_help();
    else if (name_equal(cmd, len, "clear", 5))
        sys_clear_screen();
    else if (name_equal(cmd, len, "cd", 2))
        cmd_cd(argc, argv);
    else if (name_equal(cmd, len, "exec", 4))
        cmd_exec(argc, argv, has_input);
    else if (name_equal(cmd, len, "ps", 2))
        cmd_ps();
    else if (name_equal(cmd, len, "kill", 4))
        cmd_kill(argc, argv);
    else if (name_equal(cmd, len, "export", 6))
        cmd_export(argc, argv);
    else {
        // Not a built-in: a separate program found through PATH (e.g. /bin/ls)
        bool background = argc > 1 && name_equal(argv[argc - 1], str_len(argv[argc - 1]), "&", 1);
        if (background)
            argc--;
        char full_path[PATH_MAX];
        if (find_in_path(cmd, len, full_path))
            run_program(cmd, full_path, argc, argv, has_input, background);
        else
            print_error(cmd, NULL, "command not found, try help");
    }
}

// Run one input line: stages separated by '|' run in order, each feeding its output to the next
static void run_line(void) {
    char *stages[PIPE_MAX];
    int   count = split_pipeline(stages);
    if (count < 0) {
        print_error("shell", NULL, "too many pipes");
        return;
    }

    pipe_in_len = 0;
    for (int i = 0; i < count; i++) {
        char *argv[ARG_MAX];
        int   argc = parse_args(stages[i], argv);
        if (argc == 0) {
            if (count > 1)
                print_error("shell", NULL, "syntax error near '|'");
            return; // blank line: nothing to run
        }

        bool last        = (i == count - 1);
        capture_output   = !last;
        pipe_out_len     = 0;
        output_truncated = false;
        run_command(argc, argv, i > 0);
        capture_output   = false;

        if (output_truncated)
            print_error(argv[0], NULL, "output too large for pipe, truncated");
        if (!last) {
            memcpy(pipe_in, pipe_out, pipe_out_len);
            pipe_in_len = pipe_out_len;
        }
    }
}

int main(void) {
    sys_keyboard_on();
    sys_clear_screen();
    path_load();
    print("keOSbOS shell, type help for commands\n", COLOR_TEXT);

    while (true) {
        print("keOSbOS", COLOR_PROMPT);
        sys_putchar(':', COLOR_TEXT);
        sys_puts(cwd_path, cwd_path_len, COLOR_PATH);
        print("$ ", COLOR_TEXT);

        read_line();
        run_line();
    }
    return 0;
}