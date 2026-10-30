#include <stdint.h>
#include <stdbool.h>
#include "header/user/syscall.h"
#include "header/stdlib/string.h"

#define ROOT_INODE      1
#define LINE_MAX        256
#define PATH_MAX        256
#define ARG_MAX         4
#define FIND_DEPTH_MAX  16
#define DIR_BUFFER_SIZE (32 * BLOCK_SIZE)
#define FILE_BUFFER_SIZE (128 * BLOCK_SIZE)

#define COLOR_TEXT   0x7
#define COLOR_PROMPT 0xA
#define COLOR_PATH   0x9
#define COLOR_DIR    0xB
#define COLOR_ERROR  0xC

static char     line[LINE_MAX];
static uint8_t  dir_buffer[DIR_BUFFER_SIZE];
static uint8_t  file_buffer[FILE_BUFFER_SIZE];

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

static bool is_dot_entry(struct EXT2DirectoryEntry *entry) {
    char *name = entry_name(entry);
    return name_equal(name, entry->name_len, ".", 1) || name_equal(name, entry->name_len, "..", 2);
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
    print("Commands:\n", COLOR_TEXT);
    print("  cd [dir]        change directory\n", COLOR_TEXT);
    print("  ls [dir]        list directory\n", COLOR_TEXT);
    print("  mkdir <dir>     create directory\n", COLOR_TEXT);
    print("  cat <file>      print file\n", COLOR_TEXT);
    print("  cp <src> <dst>  copy file\n", COLOR_TEXT);
    print("  rm <path>       remove file or empty directory\n", COLOR_TEXT);
    print("  mv <src> <dst>  move or rename file\n", COLOR_TEXT);
    print("  find <name>     search by name from /\n", COLOR_TEXT);
    print("  clear           clear screen\n", COLOR_TEXT);
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

static void cmd_ls(int argc, char **argv) {
    const char *path = argc >= 2 ? argv[1] : ".";
    uint32_t inode = resolve_dir(path, str_len(path));
    if (inode == 0 || !load_dir(inode)) {
        print_error("ls", path, "no such directory");
        return;
    }

    uint32_t offset = 0;
    struct EXT2DirectoryEntry *entry;
    while ((entry = next_entry(&offset)) != NULL) {
        if (is_dot_entry(entry))
            continue;
        bool is_dir = entry->file_type == EXT2_FT_DIR;
        sys_puts(entry_name(entry), entry->name_len, is_dir ? COLOR_DIR : COLOR_TEXT);
        if (is_dir)
            sys_putchar('/', COLOR_DIR);
        sys_putchar('\n', COLOR_TEXT);
    }
}

static void cmd_mkdir(int argc, char **argv) {
    if (argc < 2) {
        print_error("mkdir", NULL, "missing operand");
        return;
    }
    uint32_t    parent, name_len;
    const char *name;
    if (!split_path(argv[1], &parent, &name, &name_len)) {
        print_error("mkdir", argv[1], "invalid path");
        return;
    }

    struct EXT2DriverRequest req = {
        .buf          = NULL,
        .name         = (char*) name,
        .name_len     = name_len,
        .parent_inode = parent,
        .buffer_size  = 0,
        .is_directory = true,
    };
    int8_t ret = sys_write(&req);
    if (ret == 1)
        print_error("mkdir", argv[1], "already exists");
    else if (ret != 0)
        print_error("mkdir", argv[1], "failed (disk full?)");
}

/**
 * Read the file at path into file_buffer and set *size. Prints an error and returns false on failure.
 * read() does not report the file size, so it is found by binary searching the smallest
 * buffer_size that read() accepts. A failing probe stops before touching the data blocks.
 */
static bool load_file(const char *cmd, const char *path, uint32_t *size) {
    uint32_t    parent, name_len;
    const char *name;
    if (!split_path(path, &parent, &name, &name_len)) {
        print_error(cmd, path, "no such file");
        return false;
    }

    struct EXT2DriverRequest req = {
        .buf          = file_buffer,
        .name         = (char*) name,
        .name_len     = name_len,
        .parent_inode = parent,
        .buffer_size  = FILE_BUFFER_SIZE,
        .is_directory = false,
    };
    int8_t ret = sys_read(&req);
    if (ret == 1) {
        print_error(cmd, path, "is a directory");
        return false;
    }
    if (ret == 2) {
        print_error(cmd, path, "file too large");
        return false;
    }
    if (ret != 0) {
        print_error(cmd, path, "no such file");
        return false;
    }

    uint32_t lo = 0, hi = FILE_BUFFER_SIZE;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        req.buffer_size = mid;
        if (sys_read(&req) == 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    *size = lo;
    return true;
}

static void cmd_cat(int argc, char **argv) {
    if (argc < 2) {
        print_error("cat", NULL, "missing operand");
        return;
    }
    uint32_t size;
    if (!load_file("cat", argv[1], &size))
        return;
    sys_puts((char*) file_buffer, size, COLOR_TEXT);
    if (size > 0 && file_buffer[size - 1] != '\n')
        sys_putchar('\n', COLOR_TEXT);
}

// Copy file src to dst (a new name, or an existing directory). Returns true on success
static bool copy_file(const char *cmd, const char *src, const char *dst) {
    uint32_t size;
    if (!load_file(cmd, src, &size))
        return false;
    if (size == 0) {
        print_error(cmd, src, "cannot copy an empty file");
        return false;
    }

    uint32_t    parent, name_len;
    const char *name;
    uint32_t    dst_dir = resolve_dir(dst, str_len(dst));
    if (dst_dir != 0) {
        // Copy into the directory, keeping the source name
        uint32_t src_parent;
        if (!split_path(src, &src_parent, &name, &name_len))
            return false;
        parent = dst_dir;
    } else if (!split_path(dst, &parent, &name, &name_len)) {
        print_error(cmd, dst, "no such directory");
        return false;
    }

    struct EXT2DriverRequest req = {
        .buf          = file_buffer,
        .name         = (char*) name,
        .name_len     = name_len,
        .parent_inode = parent,
        .buffer_size  = size,
        .is_directory = false,
    };
    int8_t ret = sys_write(&req);
    if (ret == 1) {
        print_error(cmd, dst, "already exists");
        return false;
    }
    if (ret != 0) {
        print_error(cmd, dst, "failed (disk full?)");
        return false;
    }
    return true;
}

static void cmd_cp(int argc, char **argv) {
    if (argc < 3) {
        print_error("cp", NULL, "usage: cp <src> <dst>");
        return;
    }
    copy_file("cp", argv[1], argv[2]);
}

// Delete a file, or an empty directory. Returns the delete() return code
static int8_t remove_path(const char *path, bool is_directory) {
    uint32_t    parent, name_len;
    const char *name;
    if (!split_path(path, &parent, &name, &name_len))
        return 1;

    struct EXT2DriverRequest req = {
        .buf          = NULL,
        .name         = (char*) name,
        .name_len     = name_len,
        .parent_inode = parent,
        .buffer_size  = 0,
        .is_directory = is_directory,
    };
    return sys_delete(&req);
}

static void cmd_rm(int argc, char **argv) {
    if (argc < 2) {
        print_error("rm", NULL, "missing operand");
        return;
    }
    int8_t ret = remove_path(argv[1], false);
    if (ret == 1)
        ret = remove_path(argv[1], true);

    if (ret == 1)
        print_error("rm", argv[1], "no such file or directory");
    else if (ret == 2)
        print_error("rm", argv[1], "directory not empty");
    else if (ret != 0)
        print_error("rm", argv[1], "failed");
}

static void cmd_mv(int argc, char **argv) {
    if (argc < 3) {
        print_error("mv", NULL, "usage: mv <src> <dst>");
        return;
    }
    if (!copy_file("mv", argv[1], argv[2]))
        return;
    if (remove_path(argv[1], false) != 0)
        print_error("mv", argv[1], "copied but failed to remove source");
}

/* -- find -- */

static char     find_path[PATH_MAX];
static uint32_t find_count;

// Depth first search. dir_buffer is shared, so it is reloaded after visiting a child directory
static void find_in(uint32_t dir, uint32_t path_len, const char *name, uint32_t name_len, uint32_t depth) {
    if (depth >= FIND_DEPTH_MAX || !load_dir(dir))
        return;

    uint32_t offset = 0;
    struct EXT2DirectoryEntry *entry;
    while ((entry = next_entry(&offset)) != NULL) {
        if (is_dot_entry(entry))
            continue;

        uint32_t child_len = path_len + 1 + entry->name_len;
        if (child_len >= PATH_MAX)
            continue;
        find_path[path_len] = '/';
        memcpy(find_path + path_len + 1, entry_name(entry), entry->name_len);

        bool is_dir = entry->file_type == EXT2_FT_DIR;
        if (name_equal(entry_name(entry), entry->name_len, name, name_len)) {
            sys_puts(find_path, child_len, is_dir ? COLOR_DIR : COLOR_TEXT);
            sys_putchar('\n', COLOR_TEXT);
            find_count++;
        }

        if (is_dir) {
            find_in(entry->inode, child_len, name, name_len, depth + 1);
            if (!load_dir(dir))
                return;
        }
    }
}

static void cmd_find(int argc, char **argv) {
    if (argc < 2) {
        print_error("find", NULL, "missing operand");
        return;
    }
    find_count = 0;
    find_in(ROOT_INODE, 0, argv[1], str_len(argv[1]), 0);
    if (find_count == 0)
        print_error("find", argv[1], "not found");
}

/* -- Main loop -- */

static void read_line(void) {
    uint32_t len = 0;
    while (true) {
        char c = sys_getchar();
        if (c == 0)
            continue;
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

static int parse_args(char **argv) {
    int  argc = 0;
    char *p   = line;
    while (*p && argc < ARG_MAX) {
        while (*p == ' ')
            *p++ = '\0';
        if (!*p)
            break;
        argv[argc++] = p;
        while (*p && *p != ' ')
            p++;
    }
    if (*p)
        *p = '\0';
    return argc;
}

static void run_command(int argc, char **argv) {
    const char *cmd = argv[0];
    uint32_t    len = str_len(cmd);
    if (name_equal(cmd, len, "help", 4))
        cmd_help();
    else if (name_equal(cmd, len, "clear", 5))
        sys_clear_screen();
    else if (name_equal(cmd, len, "cd", 2))
        cmd_cd(argc, argv);
    else if (name_equal(cmd, len, "ls", 2))
        cmd_ls(argc, argv);
    else if (name_equal(cmd, len, "mkdir", 5))
        cmd_mkdir(argc, argv);
    else if (name_equal(cmd, len, "cat", 3))
        cmd_cat(argc, argv);
    else if (name_equal(cmd, len, "cp", 2))
        cmd_cp(argc, argv);
    else if (name_equal(cmd, len, "rm", 2))
        cmd_rm(argc, argv);
    else if (name_equal(cmd, len, "mv", 2))
        cmd_mv(argc, argv);
    else if (name_equal(cmd, len, "find", 4))
        cmd_find(argc, argv);
    else
        print_error(cmd, NULL, "command not found, try help");
}

int main(void) {
    sys_keyboard_on();
    sys_clear_screen();
    print("keOSbOS shell, type help for commands\n", COLOR_TEXT);

    while (true) {
        print("keOSbOS", COLOR_PROMPT);
        sys_putchar(':', COLOR_TEXT);
        sys_puts(cwd_path, cwd_path_len, COLOR_PATH);
        print("$ ", COLOR_TEXT);

        read_line();
        char *argv[ARG_MAX];
        int   argc = parse_args(argv);
        if (argc > 0)
            run_command(argc, argv);
    }
    return 0;
}
