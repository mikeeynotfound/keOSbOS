#include "header/user/cmdlib.h"

// cp [-r] <src> <dst> - copy a file, or a directory tree with -r.
// Folder support is a book bonus (3.4.2.2).
int main(int argc, char **argv) {
    bool recursive = false;
    int  first     = 1;

    if (argc > 1 && argv[1][0] == '-' && (argv[1][1] == 'r' || argv[1][1] == 'R') && argv[1][2] == '\0') {
        recursive = true;
        first     = 2;
    }

    if (argc < first + 2) {
        print_error("cp", NULL, recursive ? "usage: cp -r <src> <dst>" : "usage: cp <src> <dst>");
        return 1;
    }

    bool ok = recursive ? copy_tree("cp", argv[first], argv[first + 1])
                        : copy_file("cp", argv[first], argv[first + 1]);
    return ok ? 0 : 1;
}
