#include "header/user/cmdlib.h"
#include "header/stdlib/string.h"

#define FIND_DEPTH_MAX 16

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
            out_puts(find_path, child_len, is_dir ? COLOR_DIR : COLOR_TEXT);
            out_putchar('\n', COLOR_TEXT);
            find_count++;
        }

        if (is_dir) {
            find_in(entry->inode, child_len, name, name_len, depth + 1);
            if (!load_dir(dir))
                return;
        }
    }
}

// find <name> - search the whole file system (from /) for files or folders named <name>
int main(int argc, char **argv) {
    if (argc < 2) {
        print_error("find", NULL, "missing operand");
        return 1;
    }
    find_count = 0;
    find_in(ROOT_INODE, 0, argv[1], str_len(argv[1]), 0);
    if (find_count == 0) {
        print_error("find", argv[1], "not found");
        return 1;
    }
    return 0;
}
