#include "header/user/cmdlib.h"
#include "header/stdlib/string.h"

#define NAME_BUF_SIZE 256

uint8_t dir_buffer[DIR_BUFFER_SIZE];
uint8_t file_buffer[FILE_BUFFER_SIZE];
char    stdin_buffer[PIPE_BUFFER_SIZE];

/* -- Strings -- */

uint32_t str_len(const char *s) {
    uint32_t len = 0;
    while (s[len])
        len++;
    return len;
}

bool name_equal(const char *a, uint32_t a_len, const char *b, uint32_t b_len) {
    return a_len == b_len && memcmp(a, b, a_len) == 0;
}

/* -- Regex (small POSIX-like subset used by grep) -- */

#define REGEX_STEP_LIMIT 200000u

static uint32_t regex_steps;

static void regex_bit_set(uint8_t *bits, uint8_t c) {
    bits[c >> 3] |= (uint8_t) (1u << (c & 7));
}

static bool regex_bit_get(const uint8_t *bits, uint8_t c) {
    return (bits[c >> 3] & (1u << (c & 7))) != 0;
}

// End of this alternative: next unescaped '|' outside a class, or len
static uint32_t regex_branch_end(const char *pat, uint32_t len, uint32_t start) {
    bool in_class = false;
    for (uint32_t i = start; i < len; i++) {
        char c = pat[i];
        if (c == '\\') {
            i++;
            continue;
        }
        if (in_class) {
            if (c == ']')
                in_class = false;
        } else if (c == '[') {
            in_class = true;
        } else if (c == '|') {
            return i;
        }
    }
    return len;
}

bool regex_valid(const char *pat, uint32_t len) {
    uint32_t start = 0;
    while (start <= len) {
        uint32_t end = regex_branch_end(pat, len, start);
        bool have_atom = false;

        for (uint32_t i = start; i < end;) {
            char c = pat[i];
            if (c == '\\') {
                if (i + 1 >= end)
                    return false;
                i += 2;
                have_atom = true;
                continue;
            }
            if (c == '[') {
                uint32_t j = i + 1;
                if (j < end && pat[j] == '^')
                    j++;
                bool closed = false;
                while (j < end) {
                    if (pat[j] == '\\') {
                        if (j + 1 >= end)
                            return false;
                        j += 2;
                        continue;
                    }
                    if (pat[j] == ']') {
                        closed = true;
                        j++;
                        break;
                    }
                    j++;
                }
                if (!closed)
                    return false;
                i = j;
                have_atom = true;
                continue;
            }
            if (c == '*' || c == '+' || c == '?') {
                if (!have_atom)
                    return false;
                have_atom = false; // a second quantifier is invalid
                i++;
                continue;
            }
            // Anchors are not atoms and cannot be quantified
            if ((c == '^' && i == start) || (c == '$' && i + 1 == end))
                have_atom = false;
            else
                have_atom = true;
            i++;
        }

        if (end == len)
            break;
        start = end + 1;
    }
    return true;
}

struct RegexAtom {
    uint8_t  kind; // 1 = char, 2 = any, 3 = class
    char     ch;
    bool     negate;
    uint8_t  bits[32];
    uint32_t len;
};

static bool regex_parse_atom(const char *pat, uint32_t i, uint32_t end, struct RegexAtom *a) {
    memset(a, 0, sizeof(*a));
    char c = pat[i];

    if (c == '\\') {
        if (i + 1 >= end)
            return false;
        a->kind = 1;
        a->ch = pat[i + 1];
        a->len = 2;
        return true;
    }
    if (c == '.') {
        a->kind = 2;
        a->len = 1;
        return true;
    }
    if (c == '[') {
        uint32_t j = i + 1;
        a->kind = 3;
        if (j < end && pat[j] == '^') {
            a->negate = true;
            j++;
        }
        bool any = false;
        while (j < end && pat[j] != ']') {
            char lo;
            if (pat[j] == '\\') {
                if (j + 1 >= end)
                    return false;
                lo = pat[j + 1];
                j += 2;
            } else {
                lo = pat[j++];
            }
            if (j + 1 < end && pat[j] == '-' && pat[j + 1] != ']') {
                char hi;
                j++;
                if (pat[j] == '\\') {
                    if (j + 1 >= end)
                        return false;
                    hi = pat[j + 1];
                    j += 2;
                } else {
                    hi = pat[j++];
                }
                for (uint16_t v = (uint8_t) lo; v <= (uint8_t) hi; v++)
                    regex_bit_set(a->bits, (uint8_t) v);
            } else {
                regex_bit_set(a->bits, (uint8_t) lo);
            }
            any = true;
        }
        if (j >= end || !any)
            return false;
        a->len = j - i + 1;
        return true;
    }
    if (c == '*' || c == '+' || c == '?')
        return false;

    a->kind = 1;
    a->ch = c;
    a->len = 1;
    return true;
}

static bool regex_atom_matches(const struct RegexAtom *a, char c) {
    if (a->kind == 2)
        return true; // lines never contain '\n'
    if (a->kind == 1)
        return c == a->ch;
    bool in = regex_bit_get(a->bits, (uint8_t) c);
    return a->negate ? !in : in;
}

static bool regex_match_seq(const char *text, uint32_t len, uint32_t pos,
                            const char *pat, uint32_t i, uint32_t end);

static bool regex_match_branch(const char *text, uint32_t len, uint32_t pos,
                               const char *pat, uint32_t start, uint32_t end) {
    if (++regex_steps > REGEX_STEP_LIMIT)
        return false;
    if (start < end && pat[start] == '^') {
        if (pos != 0)
            return false;
        start++;
    }
    return regex_match_seq(text, len, pos, pat, start, end);
}

static bool regex_match_seq(const char *text, uint32_t len, uint32_t pos,
                            const char *pat, uint32_t i, uint32_t end) {
    if (++regex_steps > REGEX_STEP_LIMIT)
        return false;
    if (i >= end)
        return true;
    if (pat[i] == '$' && i + 1 == end)
        return pos == len;

    struct RegexAtom a;
    if (!regex_parse_atom(pat, i, end, &a))
        return false;

    uint32_t ni = i + a.len;
    char q = 0;
    if (ni < end && (pat[ni] == '*' || pat[ni] == '+' || pat[ni] == '?'))
        q = pat[ni];

    if (q == 0) {
        if (pos >= len || !regex_atom_matches(&a, text[pos]))
            return false;
        return regex_match_seq(text, len, pos + 1, pat, ni, end);
    }

    uint32_t min = (q == '+') ? 1 : 0;
    uint32_t max = (q == '?') ? 1 : len - pos;
    uint32_t count = 0;
    while (count < max && pos + count < len && regex_atom_matches(&a, text[pos + count])) {
        count++;
        if (++regex_steps > REGEX_STEP_LIMIT)
            return false;
    }
    if (count < min)
        return false;

    for (uint32_t k = count; ; k--) {
        if (regex_match_seq(text, len, pos + k, pat, ni + 1, end))
            return true;
        if (k == min)
            break;
    }
    return false;
}

bool regex_match(const char *text, uint32_t len, const char *pat, uint32_t pat_len) {
    if (!regex_valid(pat, pat_len))
        return false;
    regex_steps = 0;

    uint32_t start = 0;
    while (start <= pat_len) {
        uint32_t end = regex_branch_end(pat, pat_len, start);
        bool anchored = (start < end && pat[start] == '^');

        if (anchored) {
            if (regex_match_branch(text, len, 0, pat, start, end))
                return true;
        } else {
            for (uint32_t pos = 0; pos <= len; pos++) {
                if (regex_match_branch(text, len, pos, pat, start, end))
                    return true;
            }
        }

        if (end == pat_len)
            break;
        start = end + 1;
    }
    return false;
}

/* -- Output -- */

void out_puts(const char *s, uint32_t len, uint8_t color) {
    sys_write_stdout(s, len, color);
}

void out_putchar(char c, uint8_t color) {
    out_puts(&c, 1, color);
}

void out_print(const char *s, uint8_t color) {
    out_puts(s, str_len(s), color);
}

static void print(const char *s, uint8_t color) {
    sys_puts(s, str_len(s), color);
}

void print_error(const char *cmd, const char *arg, const char *msg) {
    print(cmd, COLOR_ERROR);
    print(": ", COLOR_ERROR);
    if (arg) {
        print(arg, COLOR_ERROR);
        print(": ", COLOR_ERROR);
    }
    print(msg, COLOR_ERROR);
    sys_putchar('\n', COLOR_ERROR);
}

/* -- Input -- */

int32_t read_stdin(void) {
    return sys_read_stdin(stdin_buffer, PIPE_BUFFER_SIZE);
}

/* -- File system -- */

uint32_t cwd_inode(void) {
    return sys_get_cwd();
}

// The "." entry lets us address a directory by its own inode
bool load_dir(uint32_t inode) {
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

char *entry_name(struct EXT2DirectoryEntry *entry) {
    return (char*) entry + sizeof(struct EXT2DirectoryEntry);
}

struct EXT2DirectoryEntry *next_entry(uint32_t *offset) {
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

bool is_dot_entry(struct EXT2DirectoryEntry *entry) {
    char *name = entry_name(entry);
    return name_equal(name, entry->name_len, ".", 1) || name_equal(name, entry->name_len, "..", 2);
}

uint32_t lookup(uint32_t dir, const char *name, uint32_t name_len, uint8_t *type) {
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

uint32_t resolve_dir(const char *path, uint32_t len) {
    uint32_t inode = (len > 0 && path[0] == '/') ? ROOT_INODE : cwd_inode();
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

bool split_path(const char *path, uint32_t *parent, const char **name, uint32_t *name_len) {
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
        *parent = cwd_inode();
    else if (slash == 1)
        *parent = ROOT_INODE;
    else
        *parent = resolve_dir(path, slash - 1);
    return *parent != 0;
}

/**
 * read() does not report the file size, so it is found by binary searching the smallest
 * buffer_size that read() accepts. A failing probe stops before touching the data blocks.
 */
static void copy_name(char *dst, const char *src, uint32_t len) {
    uint32_t n = len < NAME_BUF_SIZE - 1 ? len : NAME_BUF_SIZE - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

bool load_file_from_dir(const char *cmd, uint32_t parent, const char *name, uint32_t name_len, uint32_t *size) {
    char display[NAME_BUF_SIZE];
    copy_name(display, name, name_len);

    struct EXT2DriverRequest req = {
        .buf          = file_buffer,
        .name         = (char*) name,
        .name_len     = (uint8_t) name_len,
        .parent_inode = parent,
        .buffer_size  = FILE_BUFFER_SIZE,
        .is_directory = false,
    };
    int8_t ret = sys_read(&req);
    if (ret == 1) {
        print_error(cmd, display, "is a directory");
        return false;
    }
    if (ret == 2) {
        print_error(cmd, display, "file too large");
        return false;
    }
    if (ret != 0) {
        print_error(cmd, display, "no such file");
        return false;
    }

    // read() does not report the size: find the smallest accepted buffer.
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

bool load_file(const char *cmd, const char *path, uint32_t *size) {
    uint32_t    parent, name_len;
    const char *name;
    if (!split_path(path, &parent, &name, &name_len)) {
        print_error(cmd, path, "no such file");
        return false;
    }
    return load_file_from_dir(cmd, parent, name, name_len, size);
}

/* -- File operations shared by cp, mv and rm -- */

bool create_directory(const char *cmd, uint32_t parent, const char *name, uint32_t name_len) {
    char display[NAME_BUF_SIZE];
    copy_name(display, name, name_len);

    struct EXT2DriverRequest req = {
        .buf          = NULL,
        .name         = (char*) name,
        .name_len     = (uint8_t) name_len,
        .parent_inode = parent,
        .buffer_size  = 0,
        .is_directory = true,
    };
    int8_t ret = sys_write(&req);
    if (ret == 1) {
        print_error(cmd, display, "already exists");
        return false;
    }
    if (ret != 0) {
        print_error(cmd, display, "failed (disk full?)");
        return false;
    }
    return true;
}

bool copy_file_from_dir(const char *cmd, uint32_t src_parent, const char *src_name, uint32_t src_name_len,
                        uint32_t dst_parent, const char *dst_name, uint32_t dst_name_len) {
    uint32_t size;
    if (!load_file_from_dir(cmd, src_parent, src_name, src_name_len, &size))
        return false;

    char display[NAME_BUF_SIZE];
    copy_name(display, dst_name, dst_name_len);

    struct EXT2DriverRequest req = {
        .buf          = file_buffer,
        .name         = (char*) dst_name,
        .name_len     = (uint8_t) dst_name_len,
        .parent_inode = dst_parent,
        .buffer_size  = size, // 0 is a valid empty regular file
        .is_directory = false,
    };
    int8_t ret = sys_write(&req);
    if (ret == 1) {
        print_error(cmd, display, "already exists");
        return false;
    }
    if (ret != 0) {
        print_error(cmd, display, "failed (disk full?)");
        return false;
    }
    return true;
}

bool copy_file(const char *cmd, const char *src, const char *dst) {
    uint32_t size;
    if (!load_file(cmd, src, &size))
        return false;

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
        .name_len     = (uint8_t) name_len,
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

int8_t remove_path(const char *path, bool is_directory) {
    uint32_t    parent, name_len;
    const char *name;
    if (!split_path(path, &parent, &name, &name_len))
        return 1;

    struct EXT2DriverRequest req = {
        .buf          = NULL,
        .name         = (char*) name,
        .name_len     = (uint8_t) name_len,
        .parent_inode = parent,
        .buffer_size  = 0,
        .is_directory = is_directory,
    };
    return sys_delete(&req);
}

static void remove_path_from_dir(const char *cmd, uint32_t parent, const char *name, uint32_t name_len, bool is_dir) {
    (void) cmd;
    struct EXT2DriverRequest req = {
        .buf          = NULL,
        .name         = (char*) name,
        .name_len     = (uint8_t) name_len,
        .parent_inode = parent,
        .buffer_size  = 0,
        .is_directory = is_dir,
    };
    sys_delete(&req);
}

/* -- Recursive folder support (book bonus 3.4.2.2) -- */

#define CP_MAX_DIRS   64
#define RM_MAX_DEPTH  32

struct CopyPending {
    uint32_t src_inode;
    uint32_t dst_parent;
    char     name[NAME_BUF_SIZE];
    uint8_t  name_len;
};

static struct CopyPending copy_pending[CP_MAX_DIRS];

static bool parent_of(uint32_t inode, uint32_t *parent) {
    uint8_t  type = 0;
    uint32_t p    = lookup(inode, "..", 2, &type);
    if (p == 0 || type != EXT2_FT_DIR)
        return false;
    *parent = p;
    return true;
}

// True when `dir` is `ancestor` or one of its descendants.
static bool is_within(uint32_t dir, uint32_t ancestor) {
    uint32_t cur = dir;
    for (uint32_t i = 0; i < 64; i++) {
        if (cur == ancestor)
            return true;
        if (cur == ROOT_INODE)
            return false;
        uint32_t parent;
        if (!parent_of(cur, &parent) || parent == cur)
            return false;
        cur = parent;
    }
    return false;
}

static bool remove_dir_contents(const char *cmd, uint32_t inode, uint32_t depth) {
    if (depth >= RM_MAX_DEPTH) {
        print_error(cmd, NULL, "directory nesting too deep");
        return false;
    }
    if (!load_dir(inode)) {
        print_error(cmd, NULL, "cannot read directory");
        return false;
    }

    uint32_t offset = 0;
    struct EXT2DirectoryEntry *entry;
    while ((entry = next_entry(&offset)) != NULL) {
        if (is_dot_entry(entry))
            continue;

        // dir_buffer will be overwritten by recursion: keep a copy of the name.
        char     name[NAME_BUF_SIZE];
        uint32_t name_len = entry->name_len;
        uint32_t child    = entry->inode;
        copy_name(name, entry_name(entry), name_len);

        if (entry->file_type == EXT2_FT_DIR) {
            if (!remove_dir_contents(cmd, child, depth + 1))
                return false;
            // reload the parent table; the saved offset is still valid
            if (!load_dir(inode)) {
                print_error(cmd, NULL, "cannot read directory");
                return false;
            }
        } else if (entry->file_type != EXT2_FT_REG_FILE) {
            print_error(cmd, name, "unsupported file type");
            return false;
        }

        struct EXT2DriverRequest req = {
            .buf          = NULL,
            .name         = name,
            .name_len     = (uint8_t) name_len,
            .parent_inode = inode,
            .buffer_size  = 0,
            .is_directory = entry->file_type == EXT2_FT_DIR,
        };
        if (sys_delete(&req) != 0) {
            print_error(cmd, name, "failed to remove");
            return false;
        }
    }
    return true;
}

bool remove_tree(const char *cmd, const char *path) {
    uint32_t inode = resolve_dir(path, str_len(path));
    if (inode == 0) {
        print_error(cmd, path, "no such directory");
        return false;
    }
    if (inode == ROOT_INODE || inode == cwd_inode()) {
        print_error(cmd, path, "cannot remove root or current directory");
        return false;
    }
    if (!remove_dir_contents(cmd, inode, 0))
        return false;
    return remove_path(path, true) == 0;
}

bool copy_tree(const char *cmd, const char *src, const char *dst) {
    uint32_t src_inode = resolve_dir(src, str_len(src));
    if (src_inode == 0) {
        print_error(cmd, src, "no such directory");
        return false;
    }
    if (src_inode == ROOT_INODE) {
        print_error(cmd, src, "cannot copy root");
        return false;
    }

    uint32_t    src_parent, src_name_len;
    const char *src_name;
    if (!split_path(src, &src_parent, &src_name, &src_name_len)) {
        print_error(cmd, src, "invalid path");
        return false;
    }

    uint32_t    dst_parent, dst_name_len;
    const char *dst_name;
    uint32_t    existing = resolve_dir(dst, str_len(dst));
    if (existing != 0) {
        dst_parent  = existing;
        dst_name    = src_name;
        dst_name_len = src_name_len;
    } else if (split_path(dst, &dst_parent, &dst_name, &dst_name_len)) {
        // dst does not exist: create it with this name
    } else {
        print_error(cmd, dst, "no such directory");
        return false;
    }

    if (is_within(dst_parent, src_inode)) {
        print_error(cmd, dst, "cannot copy a directory into itself");
        return false;
    }
    if (!create_directory(cmd, dst_parent, dst_name, dst_name_len))
        return false;

    uint32_t count = 0;
    copy_pending[count].src_inode = src_inode;
    copy_pending[count].dst_parent = dst_parent;
    copy_name(copy_pending[count].name, dst_name, dst_name_len);
    copy_pending[count].name_len = (uint8_t) dst_name_len;
    count++;

    bool ok = true;
    while (count > 0 && ok) {
        struct CopyPending cur = copy_pending[--count];

        uint8_t  type = 0;
        uint32_t dst_inode = lookup(cur.dst_parent, cur.name, cur.name_len, &type);
        if (dst_inode == 0 || type != EXT2_FT_DIR) {
            print_error(cmd, cur.name, "cannot find destination directory");
            ok = false;
            break;
        }
        if (!load_dir(cur.src_inode)) {
            print_error(cmd, src, "cannot read directory");
            ok = false;
            break;
        }

        uint32_t offset = 0;
        struct EXT2DirectoryEntry *entry;
        while ((entry = next_entry(&offset)) != NULL) {
            if (is_dot_entry(entry))
                continue;

            char     name[NAME_BUF_SIZE];
            uint32_t name_len = entry->name_len;
            copy_name(name, entry_name(entry), name_len);

            if (entry->file_type == EXT2_FT_REG_FILE) {
                if (!copy_file_from_dir(cmd, cur.src_inode, name, name_len, dst_inode, name, name_len)) {
                    ok = false;
                    break;
                }
            } else if (entry->file_type == EXT2_FT_DIR) {
                if (!create_directory(cmd, dst_inode, name, name_len)) {
                    ok = false;
                    break;
                }
                if (count >= CP_MAX_DIRS) {
                    print_error(cmd, name, "too many directories");
                    ok = false;
                    break;
                }
                copy_pending[count].src_inode  = entry->inode;
                copy_pending[count].dst_parent = dst_inode;
                copy_name(copy_pending[count].name, name, name_len);
                copy_pending[count].name_len = (uint8_t) name_len;
                count++;
            } else {
                print_error(cmd, name, "unsupported file type");
                ok = false;
                break;
            }
        }
    }

    if (!ok) {
        // Best effort: remove the directory tree we created at the destination.
        uint8_t  type = 0;
        uint32_t made = lookup(dst_parent, dst_name, dst_name_len, &type);
        if (made != 0 && type == EXT2_FT_DIR)
            remove_dir_contents(cmd, made, 0);
        remove_path_from_dir(cmd, dst_parent, dst_name, dst_name_len, true);
        return false;
    }
    return true;
}
