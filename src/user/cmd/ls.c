#include "header/user/cmdlib.h"

// ls [dir] - list a directory (default: working directory)
int main(int argc, char **argv) {
    const char *path = argc >= 2 ? argv[1] : ".";
    uint32_t inode = resolve_dir(path, str_len(path));
    if (inode == 0 || !load_dir(inode)) {
        print_error("ls", path, "no such directory");
        return 1;
    }

    uint32_t offset = 0;
    struct EXT2DirectoryEntry *entry;
    while ((entry = next_entry(&offset)) != NULL) {
        if (is_dot_entry(entry))
            continue;
        bool is_dir = entry->file_type == EXT2_FT_DIR;
        out_puts(entry_name(entry), entry->name_len, is_dir ? COLOR_DIR : COLOR_TEXT);
        if (is_dir)
            out_putchar('/', COLOR_DIR);
        out_putchar('\n', COLOR_TEXT);
    }
    return 0;
}
