#include "header/user/cmdlib.h"

// rm [-r] <path> - remove a file or empty directory, or a directory tree with -r.
// Folder support is a book bonus (3.4.2.2).
int main(int argc, char **argv) {
    bool recursive = false;
    int  first     = 1;

    if (argc > 1 && argv[1][0] == '-' && (argv[1][1] == 'r' || argv[1][1] == 'R') && argv[1][2] == '\0') {
        recursive = true;
        first     = 2;
    }

    if (argc < first + 1) {
        print_error("rm", NULL, recursive ? "usage: rm -r <path>" : "missing operand");
        return 1;
    }

    const char *path = argv[first];
    int8_t ret;

    if (recursive) {
        // resolve_dir() only accepts directories; a regular file is handled by remove_path().
        uint32_t inode = resolve_dir(path, str_len(path));
        if (inode == 0) {
            bool is_file = false;
            uint32_t parent, name_len;
            const char *name;
            if (split_path(path, &parent, &name, &name_len)) {
                uint8_t type = 0;
                is_file = lookup(parent, name, name_len, &type) != 0 && type == EXT2_FT_REG_FILE;
            }
            if (!is_file) {
                print_error("rm", path, "no such file or directory");
                return 1;
            }
            ret = remove_path(path, false);
        } else {
            return remove_tree("rm", path) ? 0 : 1;
        }
    } else {
        ret = remove_path(path, false);
        if (ret == 1)
            ret = remove_path(path, true);
    }

    if (ret == 1)
        print_error("rm", path, "no such file or directory");
    else if (ret == 2)
        print_error("rm", path, "directory not empty");
    else if (ret != 0)
        print_error("rm", path, "failed");
    return ret == 0 ? 0 : 1;
}
